# 技术设计 (techdesign.md)

实现语言/栈：C++17 + 纯 Win32 API（无 Qt/无 Python）+ whisper.cpp（ggml）静态链接。
目标平台：Windows x64，CPU-only（构建机为 Broadwell AVX2，无 AVX512、无 NVIDIA GPU）。

## 1. 构建拓扑

```
CMakeLists.txt
  └ add_subdirectory(third_party/whisper.cpp)     WHISPER_BUILD_EXAMPLES=OFF
                                                   WHISPER_BUILD_TESTS=OFF
                                                   BUILD_SHARED_LIBS=OFF  (静态库)
                                                   GGML_OPENMP=ON         (不可关闭)
    └ add_executable(whisper2txt WIN32 ...)      链接 whisper, comctl32, comdlg32,
                                                 shell32, ole32, winhttp
      CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded  → 静态 CRT，不依赖 VCRUNTIME140.dll
      /utf-8                                      → 源码里的中文字面量正确编译
      /MANIFEST:EMBED /MANIFESTINPUT:src/app.manifest
```

依赖闭包（`dumpbin /dependents`）：`COMCTL32 COMDLG32 SHELL32 WINHTTP KERNEL32 USER32 GDI32 ADVAPI32 VCOMP140`。
前 8 个是系统 DLL；`VCOMP140.DLL` 是唯一非系统依赖 → 已复制到 `dist\`。

## 2. 运行时数据流

```
[UI 线程]                         [worker 线程 (std::thread)]
文件列表 / 下拉选择  ──Settings──▶ ensure_model_ctx  → http_download (WinHTTP, 可续传/切镜像)
PostMessage 回传进度/日志 ◀──────  ensure_transcriber_ctx → whisper_init_from_file_with_params
                                   for each file:
                                     decode_audio_16k_mono (miniaudio, ma_format_f32, 1ch, 16000)
                                     Transcriber::transcribe → whisper_full_with_state
                                        ├ new_segment_callback → 立即从 state 取段并累加
                                        ├ progress_callback → 百分比（mixed 模式按窗合成）
                                        └ abort_callback → 取消
                                     write_txt / write_srt  (同名同目录, UTF-8 BOM)
```

消息协议（UI 线程独占 UI，worker 只 PostMessage）：

| 消息 | 参数 | 含义 |
|---|---|---|
| `WM_APP+1` | `std::wstring*` | 追加日志行 |
| `WM_APP+2` | `ProgressInfo*` | 进度百分比 + 状态文本 |
| `WM_APP+4` | `WPARAM ok`, `LPARAM std::wstring*` | 任务结束 |

## 3. 音频解码

`src/audio.cpp` 使用 miniaudio（header-only，来自 `third_party/whisper.cpp/examples/miniaudio.h`）：

```cpp
ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, 16000);
ma_decoder_init_file(utf8_path, &cfg, &decoder);
// 循环 ma_decoder_read_pcm_frames(8192) 直到 MA_AT_END
```

- 解码 + 重采样 + 下混一步完成，无需 ffmpeg。
- 不用 `ma_decoder_get_length_in_pcm_frames` 一次分配（VBR MP3 不可靠），改为分块追加。
- 路径先 `WideCharToMultiByte(CP_UTF8)`，支持中文文件名。

## 4. Whisper 调用要点（`src/engine.cpp`）

- `whisper_context_params.use_gpu = false; flash_attn = false;`
- `whisper_init_state(ctx)` 取得 `whisper_state*` 并缓存，多次转写复用同一模型（加载 ~8.5 s）。
- `whisper_full_params`：`GREEDY`、`no_timestamps=false`（SRT 需要时间戳）、`print_*` 全 false（走回调）、`suppress_blank=true`、`suppress_nst=true`、`temperature_inc=0.2`。
- 时间戳单位：`whisper_full_get_segment_t0/t1` 返回 **厘秒**（1/100 s），×10 得 ms。

### 语言模式实现

| UI 选项 | 实现 |
|---|---|
| 中文 / 英文 | `params.language = "zh" / "en"`，一次 `whisper_full_with_state` 处理整段（whisper 内部滑窗并保留上下文，质量最好） |
| 自动检测 | `params.language = nullptr`，whisper 在开头检测一次并固定该语言 |
| 中英混合 | 按 **30 秒窗** 切分，每窗调用 `whisper_full_with_state(..., language=nullptr)`。因为每次调用传入的只是该窗样本，whisper 的内部 auto-detect 正好检测该窗的语言 |

**踩坑（重要）**：不要把 `params.detect_language = true` 用于转写 —— 该标志会让 `whisper_full_with_state` 只检测语言然后 `return 0`（不产生任何文本）。逐窗检测必须靠"每窗独立调用 + language=nullptr"实现。

**踩坑**：`whisper_full_with_state` 每次调用会 `result_all.clear()` 且重置 `state->t_last = 0`，所以每窗分段的时间是**窗内局部时间**，必须自己加 `window_base_ms_`（见 `Transcriber::window_base_ms_`）。

### 取消

`params.abort_callback` → `std::atomic<bool>::load()`；whisper 在窗口/解码循环中检查，通常 1 个窗内返回。`transcribe()` 返回后统一判 `cancel_` 决定是否报「已取消」。

## 5. 输出格式（`src/output.cpp`)

- 输入 `E:\a\歌.mp3` → 输出 `E:\a\歌.txt` 与 `E:\a\歌.srt`（**替换扩展名**，不是 `歌.mp3.txt`）。
- 均写 UTF-8 **BOM**（0xEF 0xBB 0xBF），行结束 `\r\n` → 记事本/剪映/PR 工具直接可读。
- 分段文本过滤空段与 `[BLANK_AUDIO]`；不做额外标点/换行重构。

## 6. 下载器（`src/downloader.cpp`）

WinHTTP 实现（不依赖 curl 外部进程）：

- `WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS`（HF 的 302 → CDN）
- 断点续传：`Range: bytes=<size>-\r\n`；416 → 删 `.part` 重来；206 → `total = existing + content-length`；200 → 服务器忽略 Range → 清零重下
- 完成校验：优先 HTTP `content-length`，否则用 `ModelInfo::expected_size`（turbo = 1624555275）
- 慢速保护：连续 15 s 低于 0.1 MB/s → 断开该连接并改试 `hf-mirror.com`（同一 `.part` 续传，进度不丢）
- 完成后 `.part` → `MoveFileExW` 原子改名

## 7. GUI（`src/ui.cpp`, `src/main.cpp`）

- 纯 `CreateWindowExW` 控件：ListBox（多选、`LBS_EXTENDEDSEL`）、3 个 ComboBox、3 个 Checkbox、进度条 `msctls_progress32`、只读 multiline Edit 日志。
- 字体 `Microsoft YaHei UI`，`CLEARTYPE_QUALITY`；`WM_SIZE → ui_layout()` 自适应重排。
- `DragAcceptFiles` + `WM_DROPFILES`（只接受 mp3/wav/flac）；`GetOpenFileNameW` + `OFN_ALLOWMULTISELECT|OFN_EXPLORER`（解析 `dir\0f1\0f2\0\0`）。
- `IsDialogMessageW` 提供 Tab 导航；`WM_GETMINMAXINFO` 限制最小 780×520。
- 关闭：`WM_CLOSE/WM_DESTROY` → `stop=true, cancel=true` → 轮询 `running` 最多 30 s → `join()`。
- CLI/GUI 分流：`CommandLineToArgvW`；有参数 → CLI（`--cli` 可显式）；`--gui 文件...` → GUI 预填文件列表。
- CLI 输出编码：`console_write()` 手写 UTF-8 字节 + `SetConsoleOutputCP(CP_UTF8)`；若 stdout 已重定向到文件则跳过 `AttachConsole`/`freopen("CONOUT$")`，否则日志文件会拿到乱码。

## 8. 已验证 / 未验证

已验证：MP3 与 WAV 解码、turbo 中文与中英混合准确性、mixed 逐窗检测、VAD 加速与自动下载、断点续传与镜像回退、取消与批量队列、GUI 自动化点击端到端、静态 CRT 无 VC 运行库依赖。

未做/待办见 `HANDOFF.md`。

## 9. 已知坑清单（换机器/换模型时先读）

1. `GGML_OPENMP=OFF` → 慢 28×，务必保持 ON。
2. `/exportOpenMP`、`/EXPORTOPENMP` 在本机 MSVC 14.44 + CMake 4.4 组合下**没有消除** `VCOMP140.DLL` 导入 → 采用「随 exe 分发 vcomp140.dll」。
3. HuggingFace 模型仓库名是 **`ggerganov/whisper.cpp`**，`ggml-org/whisper.cpp` 的 resolve 链接返回 **401**；VAD 模型在 `ggml-org/whisper-vad`。
4. MSVC 默认按系统 ANSI 代码页读源码，中文字面量必须 `/utf-8`（已配置）。
5. `CLEARTYPE_QUALITY`（不是 `CLEANTYPE`/`CLEARTYPE`）；`ComboBox_*`/`Button_*`/`ListBox_*` 宏需要 `<windowsx.h>`。
6. CMake 4.x 对 whisper.cpp（要求 ≥3.16）可用，configure 约 40 s，首次全量 MSBuild 约 2–4 min。
7. 若 `whisper2txt.exe` 正在运行，`cmake --build` 会报 `LNK1104 无法打开 exe` → 先杀进程。
8. UI 自动化测 Win32 Button：`InvokePattern` 不可用，改用 `NativeWindowHandle` + `SendMessageW(BM_CLICK=0x00F5)`（跨进程安全，无指针参数）。
