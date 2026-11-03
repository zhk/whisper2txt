#include "ui.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>

#include <algorithm>

static HWND make_static(UiState & ui, const wchar_t * text, HINSTANCE inst) {
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 20, ui.hwnd, nullptr, inst, nullptr);
}

static HWND make_button(UiState & ui, const wchar_t * text, HINSTANCE inst, int id, DWORD extra = BS_PUSHBUTTON) {
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | extra, 0, 0, 10, 24, ui.hwnd,
                           (HMENU) (intptr_t) id, inst, nullptr);
}

void ui_create(UiState & ui, HINSTANCE instance, HWND parent) {
    ui.hwnd = parent;

    ui.font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Microsoft YaHei UI");

    ui.btn_add = make_button(ui, L"添加 MP3 文件...", instance, 101);
    ui.btn_remove = make_button(ui, L"移除选中", instance, 102);
    ui.btn_clear = make_button(ui, L"清空列表", instance, 103);
    ui.btn_open_dir = make_button(ui, L"打开输出目录", instance, 104);

    make_static(ui, L"模型:", instance);
    ui.combo_model = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                     0, 0, 10, 200, ui.hwnd, (HMENU) 201, instance, nullptr);

    make_static(ui, L"语言:", instance);
    ui.combo_lang = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 0, 0, 10, 220,
                                    ui.hwnd, (HMENU) 202, instance, nullptr);

    make_static(ui, L"线程:", instance);
    ui.combo_threads = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 0, 0, 10, 200,
                                       ui.hwnd, (HMENU) 203, instance, nullptr);

    ui.chk_srt = make_button(ui, L"同时生成 .srt", instance, 301, BS_AUTOCHECKBOX);
    ui.chk_overwrite = make_button(ui, L"覆盖已有输出", instance, 302, BS_AUTOCHECKBOX);
    ui.chk_vad = make_button(ui, L"启用 VAD 加速 (跳过静音)", instance, 303, BS_AUTOCHECKBOX);

    ui.btn_start = make_button(ui, L"开始转写", instance, 401);
    ui.btn_cancel = make_button(ui, L"取消", instance, 402);

    ui.listbox = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_HASSTRINGS | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_EXTENDEDSEL,
                                 0, 0, 10, 10, ui.hwnd, (HMENU) 501, instance, nullptr);

    ui.progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 10, 10, ui.hwnd,
                                  (HMENU) 601, instance, nullptr);
    ui.status_label = make_static(ui, L"就绪", instance);

    ui.log = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                             0, 0, 10, 10, ui.hwnd, (HMENU) 701, instance, nullptr);

    const HWND combos[] = {ui.combo_model, ui.combo_lang, ui.combo_threads};
    const HWND buttons[] = {ui.btn_add, ui.btn_remove, ui.btn_clear, ui.btn_open_dir, ui.btn_start, ui.btn_cancel,
                            ui.chk_srt, ui.chk_overwrite, ui.chk_vad};

    for (HWND h : combos) {
        SendMessageW(h, WM_SETFONT, (WPARAM) ui.font, TRUE);
    }
    for (HWND h : buttons) {
        SendMessageW(h, WM_SETFONT, (WPARAM) ui.font, TRUE);
    }
    EnumChildWindows(ui.hwnd, [](HWND child, LPARAM font) -> BOOL {
        SendMessageW(child, WM_SETFONT, (WPARAM) font, TRUE);
        return TRUE;
    }, (LPARAM) ui.font);
}

void ui_layout(UiState & ui) {
    RECT rc;
    GetClientRect(ui.hwnd, &rc);
    const int W = rc.right;
    const int H = rc.bottom;

    const int margin = 12;
    const int right_w = 170;
    int left_w = W - right_w - margin * 3;
    if (left_w < 200) {
        left_w = 200;
    }

    int y = margin;

    MoveWindow(ui.listbox, margin, y, left_w, 130, TRUE);

    int rx = margin + left_w + margin;
    MoveWindow(ui.btn_add, rx, y, right_w, 26, TRUE);
    MoveWindow(ui.btn_remove, rx, y + 30, right_w, 26, TRUE);
    MoveWindow(ui.btn_clear, rx, y + 60, right_w, 26, TRUE);
    MoveWindow(ui.btn_open_dir, rx, y + 90, right_w, 26, TRUE);
    MoveWindow(ui.chk_srt, rx, y + 124, right_w, 22, TRUE);

    y += 130 + margin;

    int x = margin;
    MoveWindow(ui.combo_model, x, y + 22, 340, 220, TRUE);
    x += 350;
    MoveWindow(ui.combo_lang, x, y + 22, 190, 220, TRUE);
    x += 200;
    MoveWindow(ui.combo_threads, x, y + 22, 110, 200, TRUE);

    MoveWindow(ui.chk_overwrite, margin, y + 52, 150, 22, TRUE);
    MoveWindow(ui.chk_vad, margin + 160, y + 52, 260, 22, TRUE);

    MoveWindow(ui.btn_start, W - right_w * 2 - margin * 2, y + 48, right_w, 30, TRUE);
    MoveWindow(ui.btn_cancel, W - right_w - margin, y + 48, right_w, 30, TRUE);

    y += 90;

    const int bar_w = W - margin * 2 - 260;
    MoveWindow(ui.progress, margin, y, bar_w > 200 ? bar_w : 200, 22, TRUE);
    MoveWindow(ui.status_label, margin + (bar_w > 200 ? bar_w : 200) + margin, y, 250, 22, TRUE);

    y += 32;

    MoveWindow(ui.log, margin, y, W - margin * 2, H - y - margin, TRUE);
}

void ui_refresh_files(UiState & ui) {
    ListBox_ResetContent(ui.listbox);
    for (const std::wstring & f : ui.files) {
        ListBox_AddString(ui.listbox, f.c_str());
    }
}

void ui_add_files(UiState & ui, const std::vector<std::wstring> & paths) {
    for (const std::wstring & p : paths) {
        if (std::find(ui.files.begin(), ui.files.end(), p) == ui.files.end()) {
            ui.files.push_back(p);
        }
    }
    ui_refresh_files(ui);
}

void ui_append_log(UiState & ui, const std::wstring & line) {
    const int len = GetWindowTextLengthW(ui.log);
    if (len > 30000) {
        SetWindowTextW(ui.log, L"");
    }
    SendMessageW(ui.log, EM_SETSEL, (WPARAM) -1, (LPARAM) -1);
    const std::wstring text = line + L"\r\n";
    SendMessageW(ui.log, EM_REPLACESEL, FALSE, (LPARAM) text.c_str());
}

void ui_set_progress(UiState & ui, int percent, const std::wstring & status) {
    SendMessageW(ui.progress, PBM_SETRANGE32, 0, 100);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    SendMessageW(ui.progress, PBM_SETPOS, percent, 0);
    SetWindowTextW(ui.status_label, status.c_str());
}

void ui_set_busy(UiState & ui, bool busy) {
    ui.busy = busy;
    EnableWindow(ui.btn_start, !busy);
    EnableWindow(ui.btn_cancel, busy);
    EnableWindow(ui.btn_add, !busy);
    EnableWindow(ui.btn_remove, !busy);
    EnableWindow(ui.btn_clear, !busy);
    EnableWindow(ui.combo_model, !busy);
    EnableWindow(ui.combo_lang, !busy);
    EnableWindow(ui.combo_threads, !busy);
    EnableWindow(ui.chk_srt, !busy);
    EnableWindow(ui.chk_vad, !busy);
    EnableWindow(ui.chk_overwrite, !busy);
}
