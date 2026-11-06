# 技术设计 (techdesign.md)

实现语言/栈：C++17 + 纯 Win32 API（无 Qt/无 Python）+ whisper.cpp（ggml）静态链接。
目标平台：Windows x64，CPU-only（构建机为 Broadwell AVX2，无 AVX512、无 NVIDIA GPU）。

## 1. 构建拓扑

```
CMakeLists.txt
  └ add_subdirectory(third_party/whisper.cpp)     WHISPER_BUILD_EXAMPLES=OFF
                                                   WHISPER_BUILD_TESTS=OFF
                                                   WHISPER2TXT_SHARED=ON (默认) →
                                                     BUILD_SHARED_LIBS=ON, GGML_BACKEND_DL=ON,
                                                     GGML_NATIVE=OFF, GGML_CPU_ALL_VARIANTS=ON
                                                   GGML_OPENMP=ON         (不可关闭)
                                                   GGML_CUDA / GGML_VULKAN 由命令行 -D 开关
     └ add_executable(whisper2txt WIN32 ...)      链接 whisper(import lib), comctl32, comdlg32,
                                                  shell32, ole32, winhttp
       CMAKE_MSVC_RUNTIME_LIBRARY = MultiThreaded  → 静态 CRT，不依赖 VCRUNTIME140.dll
       /utf-8                                      → 源码里的中文字面量正确编译
       /MANIFEST:EMBED /MANIFESTINPUT:src/app.manifest
```

分发（dist\，整体发布）：`whisper2txt.exe` + `whisper.dll` + `ggml.dll` + `ggml-base.dll` + `ggml-cpu-*.dll`×9 + `ggml-cuda.dll`(NVIDIA 包) + `ggml-vulkan.dll`(AMD 包) + `vcomp140.dll`；CUDA 侧另带 `cublas64_13.dll` + `cublasLt64_13.dll`（仅 CUDA 构建需要）。ggml 运行时在 exe 同目录扫描 `ggml-<backend>-*.dll` 自动加载。

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
ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 16000);  // channels=0 → 保持原声道数
ma_decoder_init_file(utf8_path, &cfg, &decoder);
ma_decoder_get_data_format(&decoder, &fmt, &channels, &rate, nullptr, 0); // 解析后的输出声道数
// 循环 ma_decoder_read_pcm_frames(8192) 直到 MA_AT_END（交错 f32）
```

- 解码 + 重采样一步完成，无需 ffmpeg；**不再**让 miniaudio 直接下混（`channels=0`），因为要按声道分别算能量。
- 后处理链（`AudioPreproc`，默认开，UI 复选框 / `--no-preproc`）：
  1. 多声道时按累计能量取**较响的那个声道**（`select_loudest_channel`；关掉则平均下混）。
  2. **2 阶 Butterworth 高通**（RBJ 双二阶，`highpass_hz` 默认 100，CLI `--highpass N`，0=关）：去隆隆/风噪/直流偏置。
  3. **RMS 响度归一化**：目标 RMS = −20 dBFS（`0.1`），测整体 RMS 后统一乘增益（限幅 `[1/16, 16]`），然后做峰值限幅（若峰值 >0.98 → 压缩到 0.98）。若音频极弱（RMS < −60 dBFS）则退回峰值归一化。可 `--no-loudnorm` 退化到纯峰值归一化。
- `AudioStats` 返回声道/所选声道/RMS 前后值/峰值前后/增益，`run_files()` 打印一行「音频预处理: ... (RMS)」便于排查。
- 不用 `ma_decoder_get_length_in_pcm_frames` 一次分配（VBR MP3 不可靠），改为分块追加。
- 路径先 `WideCharToMultiByte(CP_UTF8)`，支持中文文件名。

## 4. Whisper 调用要点（`src/engine.cpp`）

- `whisper_context_params.use_gpu = cfg.use_gpu`（UI「使用 GPU 加速」复选框 / CLI `--gpu`|`--cpu`，默认启用；false 时 whisper 走 CPU 后端）；`flash_attn = use_gpu && cfg.flash_attn`（GPU 上现已默认开，large 模型提速明显）。
- 任何 whisper 调用前必须 `ggml_backend_load_all()`（`Transcriber::load()` 内 `std::call_once`），否则共享库+`GGML_BACKEND_DL` 下 registry 里没有任何后端。
- `whisper_init_state(ctx)` 取得 `whisper_state*` 并缓存，多次转写复用同一模型（加载 ~8.5 s）。
- `whisper_full_params`（所有档共用基础，`fast`/`high` 只在 strategy 与 beam 尺寸上有别）：
  - `temperature_inc=0.4`、`entropy_thold=1.5`、`logprob_thold=−1.2` —— fast 与 high 都用同一组回退阈值，保证弱音段落在 greedy 模式下也会触发温度重试。
  - `suppress_blank=true`、`suppress_nst=true`、`no_context=false`、`no_timestamps=false`（SRT 要时间戳）、`print_*` 全 false（走回调）。
  - **fast**：`WHISPER_SAMPLING_GREEDY` + `greedy.best_of=1`（其余用上述共用阈值）。
  - **high**：`WHISPER_SAMPLING_BEAM_SEARCH` + `beam_search.beam_size=5`；VAD 同时调成 `threshold 0.35 / speech_pad_ms 150 / min_silence_duration_ms 300 / max_speech_duration_s 40`。
  - `no_speech_thold` 保持 0.6（语义见 §9 #22），可通过 `EngineConfig::no_speech_thold`（CLI `--no-speech-thold`）覆盖。
- `params.initial_prompt`：术语表拼接（≤420 B）+ `carry_initial_prompt=true`（有提示词时每个内部滑窗都会 reprime，改善跨窗术语召回）。实测会同时影响**大小写**（`Review`→`review`）、**标点密度**（逗号变多）和**分段粒度**，调完必须重新 A/B。
- `params.suppress_regex`：`EngineConfig::suppress_regex`（CLI `--suppress-regex <regex>`），空＝不设。抑制匹配正则的 token（如 `[\\u3000-\\u303f]` 抑制 CJK 标点幻觉；注意会用正则开销约使速度减半，按需启）。
- **VAD 由本项目自己实现**（`Transcriber::make_vad_params()` + `Transcriber::vad_chunks()`，`src/engine.cpp`）：**whisper 的 `params.vad` 只在 `whisper_full()` 里生效，`whisper_full_with_state()` 完全忽略它**，而本项目所有调用都是 `with_state` → 设 `params.vad` 是死代码。正确做法：`whisper_vad_init_from_file_with_params()`（`load()` 里建一次并缓存 `vctx_`）→ `whisper_vad_segments_from_samples()` 取语音段（**t0/t1 单位是厘秒**，×10 得 ms）→ 合并成 ≤28 s 的块（相邻间隔 ≤1 s 才合并）→ 逐块 `whisper_full_with_state`，`window_base_ms_ = 块起始 ms`，时间戳因此留在原始轴上（不像上游 VAD 那样把静音压掉后时间漂移）。**覆盖度启发式**：语音占比 ≥85% 时**不切窗**，整段一次识别（连续录音切窗会丢上下文，实测错误率 3.27% → 4.58%）；占比 0 → 直接空输出（静音不再被幻觉成文字）。VAD 参数来自质量预设 + `VadTuning`（CLI `--vad-threshold` / `--vad-pad`，`-1`=不覆盖）。
- 后处理（`transcribe()` 末尾）：`dedupe_segments()`（归一化文本后：完全相同 → 合并时间；时间重叠 >300 ms 且互为前缀 → 取更长者）→ `apply_terms()`（`术语表` 里 `错=>对` 的替换，英文按单词边界、大小写无关，非 ASCII 按字节子串）。
- 时间戳单位：`whisper_full_get_segment_t0/t1` 返回 **厘秒**（1/100 s），×10 得 ms。

### 语言模式实现

| UI 选项 | 实现 |
|---|---|
| 中文 / 英文 | `params.language = "zh" / "en"`，一次 `whisper_full_with_state` 处理整段（whisper 内部滑窗并保留上下文，质量最好） |
| 自动检测 | `params.language = nullptr`，whisper 在开头检测一次并固定该语言 |
| 中英混合 | 按 **28/30 秒重叠窗**（hop 28 s、win 30 s）切分，每窗调用 `whisper_full_with_state(..., language=nullptr)`。因为每次调用传入的只是该窗样本，whisper 的内部 auto-detect 正好检测该窗的语言 |

**mixed 窗去重（重叠窗）**：每窗只接受 `t0 − window_base_ms_ < window_own_limit_ms_` 的分段（`window_own_limit_ms_ = min(29000, 窗长)`，见 `emit_segments()`），落在尾部 2 s 内的段交给下一窗（下一窗看得到它且右上下文更好）；窗尾重复再靠 `dedupe_segments()` 兜底。每窗还把上一窗尾部 ≤480 B 文本 + 术语表拼成 `params.initial_prompt` 传进去，补回跨窗丢失的上下文（截断时按 UTF-8 前导字节对齐，别从中间切）。

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

- 纯 `CreateWindowExW` 控件：ListBox（多选、`LBS_EXTENDEDSEL`）、4 个 ComboBox（模型 / 语言 / 线程 / 质量）、5 个 Checkbox（同时生成 .srt / 覆盖已有输出 / 启用 VAD 加速 / 使用 GPU 加速 / 音频预处理）、5 个 Button（添加 / 移除 / 清空 / 打开输出目录 / 选择术语表）、进度条 `msctls_progress32`、只读 multiline Edit 日志。
- **默认值（`WM_CREATE` 里设，`Settings` 结构体同值，CLI 不分 GUI/CLI 两套默认）**：质量 = `高精度 (beam5)`（`ComboBox_SetCurSel(combo_quality, 1)`）、VAD = 勾、音频预处理 = 勾、GPU = 勾、同时生成 .srt = 勾、覆盖已有输出 = 不勾；模型 = turbo、语言 = zh、线程 = 自动。要关 VAD/预处理用 `--no-vad` / `--no-preproc`，要更快用 `--quality fast`。
- 字体 `Microsoft YaHei UI`，`CLEARTYPE_QUALITY`；`WM_SIZE → ui_layout()` 自适应重排（4 个 `STATIC` 标签 模型/语言/线程/质量 与各自 ComboBox 同列对齐；模型下拉宽度随窗口伸缩 `[150,340]`，语言 180 / 线程 80 / 质量 130 固定；「选择术语表」按钮固定在右列 chk_srt 下方）。
- `DragAcceptFiles` + `WM_DROPFILES`（只接受 mp3/wav/flac）；`GetOpenFileNameW` + `OFN_ALLOWMULTISELECT|OFN_EXPLORER`（解析 `dir\0f1\0f2\0\0`）；术语表单选对话框见 `on_select_terms()`。
- `IsDialogMessageW` 提供 Tab 导航；`WM_GETMINMAXINFO` 限制最小 **880×520**（4 个下拉 + 2 排复选框需要这个宽度才不重叠）。
- 关闭：`WM_CLOSE/WM_DESTROY` → `stop=true, cancel=true` → 轮询 `running` 最多 30 s → `join()`。
- CLI/GUI 分流：`CommandLineToArgvW`；有参数 → CLI（`--cli` 可显式）；`--gui 文件...` → GUI 预填文件列表。
- CLI 选项与 UI 一一对应：`--model` / `--lang` / `--threads` / `--no-srt` / `--vad`（默认开，`--no-vad` 关）/ `--overwrite` / `--gpu`（默认）/ `--cpu` / `--quality fast|high`（默认 high）/ `--terms 文件` / `--no-preproc` / `--no-loudnorm` / `--highpass N` / `--vad-threshold F` / `--vad-pad N` / `--no-speech-thold F` / `--suppress-regex <regex>`。transcriber signature 含 `model|lang|threads|vad|use_gpu|quality|promptHash|termsCount|vadThreshold|vadPad|noSpeechThold|suppressRegexHash`（切换这些会重载模型；预处理只影响音频，不参与 signature）。
- 术语表解析：`load_terms_file()`（`src/main.cpp:418`）读 UTF-8（跳 BOM），`#`/`;` 注释，`A=>B` → 后处理替换，其余行拼 `initial_prompt`（≤420 B）；结果存 `App::terms_prompt / terms_repl`，按钮文案回显文件名。
- CLI 输出编码：`console_write()` 手写 UTF-8 字节 + `SetConsoleOutputCP(CP_UTF8)`；若 stdout 已重定向到文件则跳过 `AttachConsole`/`freopen("CONOUT$")`，否则日志文件会拿到乱码。
- 图标：`src/app.rc` + `src/resource.h`（`IDI_APPICON = 101`）→ `ICON "app.ico"`；`main.cpp` 里 `wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON))`、`wc.hIconSm = LoadImageW(..., SM_CXSMICON/CYSMICON)`（取不到时回落 `IDI_APPLICATION`）。`src/app.ico` 由 `scripts\gen_icon.ps1` 从仓库根 `speechtotext2.png`（512×512，白底不透明；旧图 `speechtotext.png` 已不用）生成 16/24/32/48/64/128/256 七档 PNG 压缩帧；改了原图就重跑该脚本再重新构建。

## 8. 已验证 / 未验证

已验证：MP3 与 WAV 解码、turbo 中文与中英混合准确性、mixed 逐窗检测、断点续传与镜像回退、取消与批量队列、GUI 自动化点击端到端、静态 CRT 无 VC 运行库依赖。GPU：CUDA0（RTX 5070 Ti）46× 实时、Vulkan0 回落（改名掉 ggml-cuda.dll）结果一致、CPU 模式 1.75× 实时；缺 GPU 后端 DLL 时日志提示并自动用 CPU。（注：2026-11-04 之前的「VAD 已验证」是**假验证**——`params.vad` 在 `with_state` 下无效，见 §9 #23；现在自实现后重新验证。）

识别率相关（2026-11-05 新增）：...；**同日第二轮增强**：全部模式共用 `temperature_inc=0.4` + `entropy_thold=1.5` + `logprob_thold=−1.2`（fast 也从弱音重试中受益）；`carry_initial_prompt=true` 改善多窗术语召回；`--suppress-regex` 抑制正则匹配 token；**RMS 响度归一化**替代纯峰值归一化（`stats.loudnormed`、日志打 `(RMS)`）。A/B 全表不变（default 3.27%/40× → high_vad_terms 1.31%/41×）。

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
9. **动态后端模式（当前默认）**：`GGML_BACKEND_DL` 不会自动扫描后端，必须在任何 whisper 调用前显式 `ggml_backend_load_all()`（现于 `Transcriber::load()` 内 `std::call_once`），否则 registry 里 devices=0 直接 `GGML_ASSERT(device)` 崩溃。
10. `GGML_BACKEND_DL` 与 `GGML_NATIVE=ON` 互斥 → 必须 `-DGGML_NATIVE=OFF -DGGML_CPU_ALL_VARIANTS=ON`（产出 9 个 `ggml-cpu-<arch>.dll`，运行时按 CPU 特性选）。
11. whisper.cpp 把库产物放 `build*/bin/Release/`，exe 在 `build*/Release/`——拷贝 dist 时两个目录都要扫。
12. MSBuild 的 CUDA targets 靠会话环境变量 `CUDA_PATH_V13_4` 定位工具包；新装的 CUDA 在旧 shell 会话不可见（`CudaToolkitDir '' does not exist`）→ build.ps1 已自动从安装目录设置。缺 cudart_static.lib 同因（构建阶段也要 CUDA_PATH）。
13. 禁用某后端没有通用环境变量（只有 `GGML_DISABLE_VULKAN`）；测 Vulkan 回落的办法是把 `ggml-cuda.dll` 改名。
14. UIA 对本程序的 CHECKBOX 不支持 `TogglePattern`（Win32 BUTTON 暴露为 ControlType Button），跨进程 `BM_SETCHECK(0x00F3)` 未改变状态，实测 `BM_CLICK(0x00F5)` 切换有效；读状态用 `BM_GETCHECK(0x00F0)`。日志 Edit 用 `WM_GETTEXT(0x000D)` 跨进程可读（UIA TextPattern 在本机拿不到）。
15. 共享库模式下 whisper/ggml DLL 落在 `build*/bin/Release/`，exe 在 `build*/Release/`，**直接跑 build 目录里的 exe 会因为找不到 whisper.dll 而失败** → 一律跑 `dist\whisper2txt.exe`（build.ps1 已把两者汇总到 dist）。
16. `STATIC` 标签没有自动布局：`ui_create` 里创建的 `模型:/语言:/线程:` 若不在 `ui_layout()` 里 `MoveWindow`，会留在初始 `(0,0,10,20)` 压在列表框上（曾如此）。新增控件时务必存 HWND 并在 `ui_layout()` 定位；可用 `EnumChildWindows`+`GetWindowRect`/`ScreenToClient` 脚本核对坐标与重叠（临时脚本，不入库）。
17. 若有 `dist\whisper2txt.exe` 进程在跑，`build.ps1` 删 dist 会 `Access to the path is denied`（DLL 被占用）→ build.ps1 现在先结束 Path 位于 dist 下的 whisper2txt 进程再删。
18. **音频预处理会改变 whisper 的行为**：同一 TTS 样本加归一化+高通后分段数 6→13、`三点`→`3点`（错误率不变）。所以任何预处理改动都要用 `scripts\ab_quality.ps1` 在**真实录音**上 A/B，别凭直觉开/关；预处理也不参与 transcriber signature（只改音频，不需要重载模型）。
19. **`initial_prompt` 不只是“加词”**：它会改标点密度、大小写与分段粒度（实测 `Review`→`review`、逗号变多、段更少更长）。术语表换一版就得重跑 A/B。
20. **PowerShell 5.1 按 ANSI 读 .ps1**：脚本里写中文字面量会解析报 `Missing type name after '['` 之类乱码错。`scripts\*.ps1`、`test\gui_smoke.ps1` 保持 **ASCII**（要中文请用 `[char]0xXXXX` 或从文件读），或存 UTF-8 with BOM。`tools\wer.py` 因此只输出 ASCII 关键字 `ERROR_RATE:` 供 ps1 正则取用。
21. mixed 重叠窗的**所有权 horizon（29 s）**是启发式：被丢弃的尾部段依赖下一窗重新识别，边界仍可能出现“上一段尾被切掉”的情况；`dedupe_segments()` 只能处理文本前缀包含关系，处理不了完全错乱的重复。调 `own_horizon`/`hop` 后必须重跑长样本核对（`test\zh_long.wav`）。
22. **`no_speech_thold` 是概率阈值，不是对数概率**：本版本 `state->no_speech_prob = probs[token_nosp]`（`src/whisper.cpp:7366`），判据是 `is_no_speech = no_speech_prob > no_speech_thold && avg_logprobs < logprob_thold`（`src/whisper.cpp:7791`），为真时**整个窗口的文本被丢弃且不进上下文**（`:7803/:7809`）。因此把它设成负数（网上/旧文档常见的 −0.6、−1.0 是对数概率语义）等于「凡是低置信窗一律丢掉」——在口音重/音质差的录音上表现为**成段丢字**，而不是“更保守”。本项目默认沿用库默认 0.6；要改只能按需用 `--no-speech-thold`，并在真实录音上 A/B 后才能采用。
23. **`params.vad` 对 `whisper_full_with_state()` 无效**：VAD 只写在 `whisper_full()` / `whisper_full_parallel()` 里（`src/whisper.cpp:7964`、`:7994`），本项目走 `with_state` → 曾经 UI 复选框与 `--vad` 全是**空操作**（日志里连 `loading VAD model` 都不会出现）。现在改为自实现（见 §4），并记住两点：`whisper_vad_segments_get_segment_t0/t1` 返回**厘秒**（×10=ms，`samples_to_cs`，`src/whisper.cpp:5596`）；上游 VAD 是把语音段拼接后再识别，时间戳会整体前移，本项目按原始时间轴切块所以不会漂。**验证 VAD 是否真生效要看日志里有没有 `loading VAD model` + `VAD 原始语音段: N 段`**。
24. **图标资源的两个坑**：① `rc.exe` 的 cwd 是 build 目录，`.rc` 里写的 `ICON "app.ico"` 是按**包含搜索路径**解析的 —— 现在能找到只是因为 `target_include_directories(whisper2txt PRIVATE src)`；把 .rc 或 .ico 挪位置时要同步改搜索路径。② 跨进程 `GetWindowLongPtrW(GCLP_HICON/GCLP_HICONSM)` 与 `WM_GETICON` 对**别的进程**的窗口一律返回 0（不是没设图标）；要验证图标能否加载，用 `LoadLibraryExW(..., LOAD_LIBRARY_AS_DATAFILE|LOAD_LIBRARY_AS_IMAGE_RESOURCE)` + `LoadImageW(hMod, MAKEINTRESOURCEW(101), IMAGE_ICON, size, size, 0)` 在本进程试（PNG 压缩的 ICO 帧在 Win7+ 可被任意尺寸缩放，实测 16–256 与 40 都 OK）。Explorer 里图标不刷新是**图标缓存**问题，与 exe 无关。

## 10. 代码风格

- `src/` 全部使用 **Allman 大括号**（左大括号单独一行），4 空格缩进，制表符不用；根目录 `.clang-format` 是权威配置（`ColumnLimit: 0` 保留既有换行、`SortIncludes: false` 不改包含顺序）。
- 格式化命令（PATH 上的 `clang-format` 是 6.0.0，太旧会忽略新选项）：
  `& "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\LLVM\bin\clang-format.exe" -i -style=file src\*.cpp src\*.h`
- `third_party/whisper.cpp/` 不格式化、不改动。
