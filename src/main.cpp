#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio.h"
#include "downloader.h"
#include "engine.h"
#include "models.h"
#include "output.h"
#include "ui.h"
#include "util.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#define WM_APP_LOG       (WM_APP + 1)
#define WM_APP_PROGRESS  (WM_APP + 2)
#define WM_APP_FILE_DONE (WM_APP + 3)
#define WM_APP_JOB_DONE  (WM_APP + 4)

struct ProgressInfo {
    int percent;
    int64_t a;
    int64_t b;
    std::wstring text;
};

struct App {
    UiState ui;
    std::atomic<bool> cancel{false};
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::thread worker;
    std::unique_ptr<Transcriber> transcriber;
    std::string loaded_signature;
    std::wstring last_output_file;
};

struct Settings {
    int model_slot = SLOT_TURBO;
    LangMode lang = LangMode::Zh;
    int threads = 0;
    bool srt = true;
    bool vad = false;
    bool overwrite = false;
    std::vector<std::wstring> files;
};

static std::vector<std::wstring> g_prefill_files;

static void post_log(App * app, const std::wstring & line) {
    if (app == nullptr || app->ui.hwnd == nullptr) {
        return;
    }
    PostMessageW(app->ui.hwnd, WM_APP_LOG, 0, (LPARAM) new std::wstring(line));
}

static void post_progress(App * app, int percent, int64_t a, int64_t b, const std::wstring & text) {
    if (app == nullptr || app->ui.hwnd == nullptr) {
        return;
    }
    ProgressInfo * info = new ProgressInfo{percent, a, b, text};
    PostMessageW(app->ui.hwnd, WM_APP_PROGRESS, 0, (LPARAM) info);
}

static std::wstring fmt_mb(int64_t bytes) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%.2f GB", (double) bytes / (1024.0 * 1024.0 * 1024.0));
    return std::wstring(buf);
}

struct JobContext {
    std::function<void(const std::wstring &)> log;
    std::function<void(int percent, int64_t a, int64_t b, const std::wstring & text)> progress;
    std::atomic<bool> * cancel;
    std::unique_ptr<Transcriber> * transcriber;
    std::string * signature;
    std::wstring * last_output;
};

static bool ensure_model_ctx(JobContext & ctx, const ModelInfo & info, std::wstring & err) {
    if (model_ready(info)) {
        return true;
    }

    const std::wstring path = model_path(info);
    ctx.log(L"模型缺失，开始下载: " + std::wstring(info.filename));
    ctx.log(L"保存到: " + path);

    const auto started = std::chrono::steady_clock::now();
    auto last_post = started;

    const bool ok = http_download(
        info.url, info.mirror_url, path, *ctx.cancel,
        [&](int64_t done, int64_t total) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_post < std::chrono::milliseconds(200)) {
                return;
            }
            last_post = now;
            const int percent = total > 0 ? (int) (done * 100 / total) : 0;
            std::wstring text = L"下载模型 ";
            text += std::to_wstring(percent);
            text += L"%  ";
            text += fmt_mb(done);
            if (total > 0) {
                text += L" / " + fmt_mb(total);
            }
            ctx.progress(percent, done, total, text);
        },
        err, info.expected_size);

    if (!ok) {
        return false;
    }

    ctx.log(L"模型下载完成: " + path);
    return true;
}

static bool ensure_transcriber_ctx(JobContext & ctx, Settings & st, std::wstring & err) {
    const std::string signature = std::to_string(st.model_slot) + "|" + std::to_string((int) st.lang) + "|" +
                                  std::to_string(st.threads) + "|" + std::to_string(st.vad ? 1 : 0);

    if (*ctx.transcriber != nullptr && *ctx.signature == signature) {
        return true;
    }

    const ModelInfo & info = model_for_slot(st.model_slot);
    EngineConfig cfg;
    cfg.model_path = model_path(info);
    cfg.n_threads = st.threads;
    cfg.lang = st.lang;
    cfg.vad = st.vad;
    if (st.vad) {
        cfg.vad_model_path = model_path(vad_model_info());
    }

    ctx.log(L"加载模型: " + std::wstring(info.filename) + L" ...");
    const auto started = std::chrono::steady_clock::now();

    auto engine = std::make_unique<Transcriber>();
    engine->reset_cancel();
    if (!engine->load(cfg, ctx.log, err)) {
        return false;
    }

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    wchar_t buf[64];
    swprintf(buf, 64, L"模型加载完成 (%.1f 秒)", secs);
    ctx.log(buf);

    *ctx.transcriber = std::move(engine);
    *ctx.signature = signature;
    return true;
}

static bool run_files(JobContext & ctx, Settings st, std::wstring & job_message) {
    bool job_ok = true;

    do {
        const ModelInfo & info = model_for_slot(st.model_slot);
        std::wstring err;

        if (!ensure_model_ctx(ctx, info, err)) {
            job_ok = false;
            job_message = L"模型不可用: " + err;
            break;
        }

        if (st.vad) {
            const ModelInfo & vad = vad_model_info();
            if (!model_ready(vad)) {
                if (!ensure_model_ctx(ctx, vad, err)) {
                    st.vad = false;
                    ctx.log(L"VAD 模型下载失败，改用无 VAD 模式: " + err);
                }
            }
        }

        if (!ensure_transcriber_ctx(ctx, st, err)) {
            job_ok = false;
            job_message = err;
            break;
        }

        int ok_count = 0;
        int fail_count = 0;

        for (const std::wstring & file : st.files) {
            if (ctx.cancel->load()) {
                job_message = L"任务已取消";
                job_ok = false;
                break;
            }

            const std::wstring out_txt = replace_extension(file, L".txt");
            const std::wstring out_srt = replace_extension(file, L".srt");
            const std::wstring name = file_name_of(file);

            const DWORD attr_txt = GetFileAttributesW(out_txt.c_str());
            if (attr_txt != INVALID_FILE_ATTRIBUTES && !st.overwrite) {
                ctx.log(L"跳过 (已存在 " + out_txt + L")，可勾选覆盖已有输出");
                ++fail_count;
                continue;
            }

            ctx.progress(0, 0, 0, L"解码音频: " + name);
            std::vector<float> samples;
            if (!decode_audio_16k_mono(file, samples, err)) {
                ctx.log(L"[失败] " + name + L": " + err);
                ++fail_count;
                continue;
            }

            const int64_t duration_ms = (int64_t) samples.size() * 1000 / WHISPER_SAMPLE_RATE;
            ctx.log(L"开始转写: " + name + L"  时长 " + fmt_clock(duration_ms));

            std::vector<Segment> segments;
            const auto started = std::chrono::steady_clock::now();

            (*ctx.transcriber)->reset_cancel();
            const bool ok = (*ctx.transcriber)->transcribe(
                samples, segments,
                [&](int percent, int64_t done_ms, int64_t total_ms) {
                    wchar_t buf[128];
                    swprintf(buf, 128, L"转写 %d%%  %s / %s", percent, fmt_clock(done_ms).c_str(),
                             fmt_clock(total_ms).c_str());
                    ctx.progress(percent, done_ms, total_ms, buf);
                },
                ctx.log, err);

            if (!ok) {
                ctx.log(L"[失败] " + name + L": " + err);
                ++fail_count;
                if (ctx.cancel->load()) {
                    job_message = L"任务已取消";
                    job_ok = false;
                    break;
                }
                continue;
            }

            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            const double ratio = secs > 0.0 ? (double) duration_ms / 1000.0 / secs : 0.0;

            if (segments.empty()) {
                ctx.log(L"[警告] " + name + L" 未检测到语音内容");
            }

            if (!write_txt(out_txt, segments, err)) {
                ctx.log(L"[失败] 写入 " + out_txt + L": " + err);
                ++fail_count;
                continue;
            }

            if (st.srt && !write_srt(out_srt, segments, err)) {
                ctx.log(L"[失败] 写入 " + out_srt + L": " + err);
            }

            wchar_t buf[256];
            swprintf(buf, 256, L"完成: %s  (%d 段, 用时 %.1f 秒, 速度 %.2fx 实时)", name.c_str(), (int) segments.size(),
                     secs, ratio);
            ctx.log(buf);
            ctx.log(L"输出: " + out_txt + (st.srt ? L"  " + out_srt : L""));

            *ctx.last_output = out_txt;
            ++ok_count;
        }

        wchar_t buf[128];
        swprintf(buf, 128, L"任务结束: 成功 %d, 失败/跳过 %d", ok_count, fail_count);
        if (job_message.empty()) {
            job_message = buf;
        }
    } while (false);

    return job_ok;
}

static void run_job(App * app, Settings st) {
    app->running.store(true);

    JobContext ctx;
    ctx.log = [&](const std::wstring & line) { post_log(app, line); };
    ctx.progress = [&](int percent, int64_t a, int64_t b, const std::wstring & text) {
        post_progress(app, percent, a, b, text);
    };
    ctx.cancel = &app->cancel;
    ctx.transcriber = &app->transcriber;
    ctx.signature = &app->loaded_signature;
    ctx.last_output = &app->last_output_file;

    std::wstring message;
    const bool ok = run_files(ctx, st, message);

    app->running.store(false);
    PostMessageW(app->ui.hwnd, WM_APP_JOB_DONE, ok ? 1 : 0, (LPARAM) new std::wstring(message));
}

static std::wstring read_combo_line(HWND combo) {
    const int sel = (int) SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (sel < 0) {
        return std::wstring();
    }
    const int len = (int) SendMessageW(combo, CB_GETLBTEXTLEN, sel, 0);
    std::wstring text((size_t) len, L'\0');
    SendMessageW(combo, CB_GETLBTEXT, sel, (LPARAM) text.data());
    return text;
}

static Settings read_settings(App * app) {
    Settings st;
    st.model_slot = (int) SendMessageW(app->ui.combo_model, CB_GETCURSEL, 0, 0);
    if (st.model_slot < 0) {
        st.model_slot = SLOT_TURBO;
    }

    const int lang_sel = (int) SendMessageW(app->ui.combo_lang, CB_GETCURSEL, 0, 0);
    switch (lang_sel) {
        case 1: st.lang = LangMode::En; break;
        case 2: st.lang = LangMode::Auto; break;
        case 3: st.lang = LangMode::Mixed; break;
        default: st.lang = LangMode::Zh; break;
    }

    const int thread_sel = (int) SendMessageW(app->ui.combo_threads, CB_GETCURSEL, 0, 0);
    if (thread_sel > 0) {
        const std::wstring t = read_combo_line(app->ui.combo_threads);
        st.threads = _wtoi(t.c_str());
    } else {
        st.threads = 0;
    }

    st.srt = (SendMessageW(app->ui.chk_srt, BM_GETCHECK, 0, 0) == BST_CHECKED);
    st.vad = (SendMessageW(app->ui.chk_vad, BM_GETCHECK, 0, 0) == BST_CHECKED);
    st.overwrite = (SendMessageW(app->ui.chk_overwrite, BM_GETCHECK, 0, 0) == BST_CHECKED);
    st.files = app->ui.files;
    return st;
}

static void on_files_dropped(App * app, HDROP hdrop) {
    const UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::wstring> added;
    wchar_t path[MAX_PATH * 4];

    for (UINT i = 0; i < count; ++i) {
        const UINT len = DragQueryFileW(hdrop, i, path, (UINT) (sizeof(path) / sizeof(path[0])));
        if (len == 0) {
            continue;
        }
        std::wstring p(path, len);
        DWORD attr = GetFileAttributesW(p.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            continue;
        }
        const size_t dot = p.find_last_of(L'.');
        if (dot == std::wstring::npos) {
            continue;
        }
        std::wstring ext = p.substr(dot + 1);
        for (wchar_t & c : ext) {
            c = (wchar_t) towlower(c);
        }
        if (ext != L"mp3" && ext != L"wav" && ext != L"flac") {
            continue;
        }
        added.push_back(p);
    }

    DragFinish(hdrop);

    if (!added.empty()) {
        ui_add_files(app->ui, added);
        ui_append_log(app->ui, L"添加 " + std::to_wstring(added.size()) + L" 个音频文件");
    }
}

static void on_add_files(App * app) {
    static wchar_t buffer[32768];
    buffer[0] = 0;

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = app->ui.hwnd;
    ofn.lpstrFilter = L"音频文件 (*.mp3;*.wav;*.flac)\0*.mp3;*.wav;*.flac\0所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = (DWORD) (sizeof(buffer) / sizeof(buffer[0]));
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle = L"选择 MP3 文件";

    if (!GetOpenFileNameW(&ofn)) {
        const DWORD err = CommDlgExtendedError();
        if (err != 0) {
            wchar_t buf[64];
            swprintf(buf, 64, L"文件对话框错误 (code %lu)", err);
            ui_append_log(app->ui, buf);
        }
        return;
    }

    std::vector<std::wstring> added;
    wchar_t * cursor = buffer;

    const std::wstring first(cursor);
    cursor += first.size() + 1;

    if (*cursor == 0) {
        added.push_back(first);
    } else {
        while (*cursor != 0) {
            std::wstring name(cursor);
            cursor += name.size() + 1;
            std::wstring full = first;
            if (!full.empty() && full.back() != L'\\') {
                full += L'\\';
            }
            full += name;
            added.push_back(full);
        }
    }

    if (!added.empty()) {
        ui_add_files(app->ui, added);
        ui_append_log(app->ui, L"添加 " + std::to_wstring(added.size()) + L" 个音频文件");
    }
}

static void on_remove_selected(App * app) {
    std::vector<std::wstring> keep;
    const int count = (int) app->ui.files.size();
    for (int i = 0; i < count; ++i) {
        if (ListBox_GetSel(app->ui.listbox, i) <= 0) {
            keep.push_back(app->ui.files[i]);
        }
    }
    app->ui.files.swap(keep);
    ui_refresh_files(app->ui);
}

static void on_open_output_dir(App * app) {
    std::wstring target = app->last_output_file;
    if (target.empty()) {
        for (const std::wstring & f : app->ui.files) {
            const std::wstring t = replace_extension(f, L".txt");
            if (GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES) {
                target = t;
                break;
            }
        }
    }

    if (!target.empty()) {
        std::wstring cmd = L"/select,\"" + target + L"\"";
        ShellExecuteW(app->ui.hwnd, L"open", L"explorer.exe", cmd.c_str(), nullptr, SW_SHOWNORMAL);
        return;
    }

    const std::wstring dir = models_dir();
    ShellExecuteW(app->ui.hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void start_job(App * app) {
    if (app->running.load()) {
        return;
    }
    if (app->ui.files.empty()) {
        ui_append_log(app->ui, L"请先添加 MP3 文件 (可多选或拖放)");
        return;
    }

    Settings st = read_settings(app);
    app->cancel.store(false);
    ui_set_busy(app->ui, true);
    ui_set_progress(app->ui, 0, L"准备中...");

    if (app->worker.joinable()) {
        app->worker.join();
    }
    app->worker = std::thread(run_job, app, st);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App * app = reinterpret_cast<App *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto * cs = reinterpret_cast<CREATESTRUCTW *>(lp);
            app = reinterpret_cast<App *>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->ui.hwnd = hwnd;

            ui_create(app->ui, cs->hInstance, hwnd);

            const ModelInfo models[3] = {model_for_slot(0), model_for_slot(1), model_for_slot(2)};
            for (const ModelInfo & m : models) {
                ComboBox_AddString(app->ui.combo_model, m.label);
            }
            ComboBox_SetCurSel(app->ui.combo_model, 0);

            ComboBox_AddString(app->ui.combo_lang, L"中文 (zh)");
            ComboBox_AddString(app->ui.combo_lang, L"英文 (en)");
            ComboBox_AddString(app->ui.combo_lang, L"自动检测 (整篇一种语言)");
            ComboBox_AddString(app->ui.combo_lang, L"中英混合 (逐 30 秒窗自动检测)");
            ComboBox_SetCurSel(app->ui.combo_lang, 0);

            const wchar_t * threads[] = {L"自动", L"2", L"4", L"8", L"12", L"16", L"18", L"24", L"32", L"36"};
            for (const wchar_t * t : threads) {
                ComboBox_AddString(app->ui.combo_threads, t);
            }
            ComboBox_SetCurSel(app->ui.combo_threads, 0);

            Button_SetCheck(app->ui.chk_srt, BST_CHECKED);
            Button_SetCheck(app->ui.chk_overwrite, BST_UNCHECKED);
            Button_SetCheck(app->ui.chk_vad, BST_UNCHECKED);

            DragAcceptFiles(hwnd, TRUE);
            ui_layout(app->ui);
            ui_set_busy(app->ui, false);

            ui_append_log(app->ui, L"Whisper 本地转写工具 (whisper.cpp + large-v3-turbo)");
            ui_append_log(app->ui, L"拖放 MP3 到窗口，或点“添加 MP3 文件...”，输出同名 .txt (和 .srt)");
            ui_append_log(app->ui, L"模型缓存目录: " + models_dir());

            if (!g_prefill_files.empty()) {
                ui_add_files(app->ui, g_prefill_files);
                ui_append_log(app->ui, L"已预载 " + std::to_wstring(g_prefill_files.size()) + L" 个文件");
            }
            return 0;
        }

        case WM_SIZE:
            if (app != nullptr) {
                ui_layout(app->ui);
            }
            return 0;

        case WM_GETMINMAXINFO: {
            auto * mmi = reinterpret_cast<MINMAXINFO *>(lp);
            mmi->ptMinTrackSize.x = 780;
            mmi->ptMinTrackSize.y = 520;
            return 0;
        }

        case WM_DROPFILES:
            if (app != nullptr) {
                on_files_dropped(app, (HDROP) wp);
            }
            return 0;

        case WM_COMMAND: {
            if (app == nullptr) {
                return 0;
            }
            const int id = (int) LOWORD(wp);
            switch (id) {
                case 101:
                    on_add_files(app);
                    return 0;
                case 102:
                    on_remove_selected(app);
                    return 0;
                case 103:
                    app->ui.files.clear();
                    ui_refresh_files(app->ui);
                    return 0;
                case 104:
                    on_open_output_dir(app);
                    return 0;
                case 401:
                    start_job(app);
                    return 0;
                case 402:
                    app->cancel.store(true);
                    ui_append_log(app->ui, L"正在取消 ...");
                    return 0;
                default:
                    return 0;
            }
        }

        case WM_APP_LOG: {
            auto * line = reinterpret_cast<std::wstring *>(lp);
            if (app != nullptr && line != nullptr) {
                ui_append_log(app->ui, *line);
            }
            delete line;
            return 0;
        }

        case WM_APP_PROGRESS: {
            auto * info = reinterpret_cast<ProgressInfo *>(lp);
            if (app != nullptr && info != nullptr) {
                ui_set_progress(app->ui, info->percent, info->text);
            }
            delete info;
            return 0;
        }

        case WM_APP_JOB_DONE: {
            auto * message = reinterpret_cast<std::wstring *>(lp);
            if (app != nullptr) {
                ui_set_busy(app->ui, false);
                if (wp != 0) {
                    ui_set_progress(app->ui, 100, L"完成");
                } else {
                    ui_set_progress(app->ui, 0, L"已结束");
                }
                if (message != nullptr && !message->empty()) {
                    ui_append_log(app->ui, *message);
                }
            }
            delete message;
            return 0;
        }

        case WM_CLOSE:
            if (app != nullptr) {
                app->stop.store(true);
                app->cancel.store(true);
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (app != nullptr) {
                app->stop.store(true);
                app->cancel.store(true);
                for (int i = 0; i < 300 && app->running.load(); ++i) {
                    Sleep(100);
                }
                if (app->worker.joinable()) {
                    app->worker.join();
                }
            }
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static std::atomic<bool> * g_cli_cancel = nullptr;

static BOOL WINAPI cli_ctrl_handler(DWORD type) {
    (void) type;
    if (g_cli_cancel != nullptr) {
        g_cli_cancel->store(true);
    }
    return TRUE;
}

static void console_write(const std::wstring & text) {
    const std::string bytes = u8(text);
    fwrite(bytes.data(), 1, bytes.size(), stdout);
    fflush(stdout);
}

static int run_cli_mode() {
    const DWORD output_type = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
    if (output_type == FILE_TYPE_CHAR) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
            AllocConsole();
        }
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    SetConsoleOutputCP(CP_UTF8);

    int argc = 0;
    LPWSTR * argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return 2;
    }

    Settings st;
    std::atomic<bool> cancel(false);
    std::unique_ptr<Transcriber> transcriber;
    std::string signature;
    std::wstring last_output;
    g_cli_cancel = &cancel;
    SetConsoleCtrlHandler(cli_ctrl_handler, TRUE);

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--cli") {
            continue;
        }
        if (a == L"--model" && i + 1 < argc) {
            ++i;
            if (_wcsicmp(argv[i], L"medium") == 0) {
                st.model_slot = SLOT_MEDIUM;
            } else if (_wcsicmp(argv[i], L"large") == 0 || _wcsicmp(argv[i], L"large-v3") == 0) {
                st.model_slot = SLOT_LARGE;
            } else {
                st.model_slot = SLOT_TURBO;
            }
            continue;
        }
        if (a == L"--lang" && i + 1 < argc) {
            ++i;
            if (_wcsicmp(argv[i], L"en") == 0) {
                st.lang = LangMode::En;
            } else if (_wcsicmp(argv[i], L"auto") == 0) {
                st.lang = LangMode::Auto;
            } else if (_wcsicmp(argv[i], L"mixed") == 0) {
                st.lang = LangMode::Mixed;
            } else {
                st.lang = LangMode::Zh;
            }
            continue;
        }
        if (a == L"--threads" && i + 1 < argc) {
            ++i;
            st.threads = _wtoi(argv[i]);
            continue;
        }
        if (a == L"--no-srt") {
            st.srt = false;
            continue;
        }
        if (a == L"--vad") {
            st.vad = true;
            continue;
        }
        if (a == L"--overwrite") {
            st.overwrite = true;
            continue;
        }
        if (!a.empty() && a[0] == L'-') {
            console_write(L"未知选项: " + a + L"\n");
            continue;
        }
        st.files.push_back(a);
    }

    if (st.files.empty()) {
        console_write(L"用法: whisper2txt.exe [选项] 文件1.mp3 [文件2.mp3 ...]\n");
        console_write(L"  --model turbo|medium|large   默认 turbo\n");
        console_write(L"  --lang zh|en|auto|mixed      默认 zh (mixed = 每 30 秒窗自动检测语言)\n");
        console_write(L"  --threads N   --no-srt   --vad   --overwrite\n");
        console_write(L"输出: 与输入同目录同名的 .txt (和 .srt)\n");
        return 2;
    }

    JobContext ctx;
    ctx.log = [](const std::wstring & line) { console_write(line + L"\n"); };
    ctx.progress = [](int percent, int64_t a, int64_t b, const std::wstring & text) {
        (void) percent;
        (void) a;
        (void) b;
        std::wstring padded = text;
        while (padded.size() < 60) {
            padded += L' ';
        }
        console_write(L"\r" + padded);
    };
    ctx.cancel = &cancel;
    ctx.transcriber = &transcriber;
    ctx.signature = &signature;
    ctx.last_output = &last_output;

    std::wstring message;
    const bool ok = run_files(ctx, st, message);

    console_write(L"\n" + message + L"\n");
    g_cli_cancel = nullptr;
    return ok ? 0 : 1;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
    int probe_argc = 0;
    LPWSTR * probe_argv = CommandLineToArgvW(GetCommandLineW(), &probe_argc);
    bool cli = false;
    if (probe_argv != nullptr) {
        if (probe_argc > 1 && _wcsicmp(probe_argv[1], L"--gui") == 0) {
            for (int i = 2; i < probe_argc; ++i) {
                const std::wstring a = probe_argv[i];
                if (!a.empty() && a[0] != L'-') {
                    g_prefill_files.push_back(a);
                }
            }
            cli = false;
        } else {
            cli = probe_argc > 1;
        }
        LocalFree(probe_argv);
    }
    if (cli) {
        return run_cli_mode();
    }

    SetProcessDPIAware();

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    App app;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
    wc.lpszClassName = L"WhisperTxtWindow";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    const int width = 900;
    const int height = 640;
    RECT rc{0, 0, width, height};
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);

    HWND hwnd = CreateWindowExW(0, L"WhisperTxtWindow", L"Whisper 转文字 (MP3 → TXT/SRT)",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                                rc.bottom - rc.top, nullptr, nullptr, instance, &app);
    if (hwnd == nullptr) {
        return 1;
    }

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (app.worker.joinable()) {
        app.worker.join();
    }
    app.transcriber.reset();
    if (app.ui.font != nullptr) {
        DeleteObject(app.ui.font);
    }
    return (int) msg.wParam;
}
