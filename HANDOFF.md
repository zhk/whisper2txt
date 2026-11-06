# 交接说明 (HANDOFF.md)

用途：换 AI 模型 / 换机器时，让接手者 **10 分钟内恢复上下文并继续**。

## 一句话现状

功能已全部可用的阶段：`whisper2txt`（C++17 + 纯 Win32 + whisper.cpp large-v3-turbo）已完成 GUI + CLI + MP3 解码 + 同名 `.txt`/`.srt` 输出 + 模型自动下载续传/镜像回退 + 取消 + 批量 + VAD 加速，并已端到端实测通过。**2026-11-04 起改为动态库分发**：`whisper.dll/ggml.dll/ggml-base.dll` + 运行时按需加载的后端 DLL（`ggml-cuda.dll` / `ggml-vulkan.dll` / 9 个 `ggml-cpu-*.dll`），一次构建同时支持 NVIDIA(CUDA 13.4) 与 AMD(Vulkan)，`whisper2txt.exe` 仅 ~800 KB；`use_gpu=true`（无 GPU 自动回落 CPU），CUDA0 实测优先、缺 cuda DLL 自动落 Vulkan0。**同日新增识别率调优套件**：音频预处理（较响声道 + 100 Hz 高通 + 峰值归一化）、`质量` 两档（fast greedy / high beam5 + 阈值与 VAD 调参）、术语表（`initial_prompt` + `错=>对` 后处理替换）、分段去重、mixed 28/30 重叠窗 + 跨窗上下文、GPU `flash_attn`，以及 A/B 评分工具（`scripts\ab_quality.ps1` + `tools\wer.py`）。**同日复查时发现并修掉两个真实 bug**：(a) VAD 从来是空操作（whisper 的 `params.vad` 只在 `whisper_full()` 生效，我们全用 `whisper_full_with_state()`）→ 改成自己用 `whisper_vad_*` 切块（含“语音覆盖 ≥85% 就不切窗”的启发式，时间戳保持原始轴）；(b) high 预设里 `no_speech_thold=-0.6` 是错误语义（该字段是概率阈值，负数=低置信窗整窗丢字）→ 改回库默认 0.6 并加 `--no-speech-thold`。实测：30 s 纯静音从 1 行幻觉变 0 行；large-v3（已缓存 3.09 GB）+ 术语表把衰减 28 dB 的差录音从 3.27% 降到 **0.65%**（11.9× 实时）。**默认档位已改（2026-11-05）**：质量默认 **高精度 (beam5)**、VAD 加速默认 **勾选**、音频预处理默认勾选（GUI `WM_CREATE` 与 `Settings` 结构体同值；CLI 同样默认 high+VAD，关闭用 `--quality fast` / `--no-vad` / `--no-preproc`）。实测默认档在 45 s 样本上 40.2× 实时、错误率与 high+VAD 一致（3.27%，加术语表 1.31%）；纯静音输出 0 行。A/B 脚本新增 `default_run` 配置（不带任何 flag，即出厂默认）。**本次改动未 commit**（`git status`：8 个 M：`src/engine.*`、`src/main.cpp` + 5 份文档）。上一个 commit 是 `140134d Improve quality`（第一轮调优套件），再前面是 `ba55db6 GPU acceleration enable`。

## 接手必读顺序

1. 本文件
2. `techdesign.md` §9「已知坑清单」（23 条，全部会踩）
3. `progress.md` 实测数据表 + 遗留清单
4. 需要改代码时：`src/engine.cpp`（whisper 调用细节）、`src/main.cpp`（GUI/CLI/线程/消息）

## 当前工作区状态

```
已生成产物（动态库方案，2026-11-04，build_gpu\）：
  dist\whisper2txt.exe          816,640 B   （链接 whisper.dll，不再内嵌后端）
  dist\whisper.dll / ggml.dll / ggml-base.dll
  dist\ggml-cpu-*.dll         9 个（sse42/sandybridge/haswell/alderlake/icelake/skylakex/cascadelake/cannonlake/x64）
  dist\ggml-cuda.dll          155,000,832 B（NVIDIA 发行包）
  dist\ggml-vulkan.dll         42,290,688 B（AMD/通用发行包）
  dist\cublas64_13.dll + cublasLt64_13.dll（CUDA 运行时，仅 CUDA 包需要，合计 ~548 MB）
  dist\vcomp140.dll           212,920 B     （必须与 exe 同目录）
构建目录：build_gpu\（CUDA+Vulkan+共享库）；build\（纯 CPU 静态，若用 -Static）
模型缓存（不在仓库）：
  %LOCALAPPDATA%\WhisperTxt\models\ggml-large-v3-turbo.bin   1,624,555,275 B
  %LOCALAPPDATA%\WhisperTxt\models\ggml-silero-v6.2.0.bin       885,098 B
第三方：third_party\whisper.cpp  （git clone --depth 1 ggml-org/whisper.cpp，已被 .gitignore 忽略）
测试：test\  （样本 wav/mp3、输出 txt/srt、out_*.log、gui_smoke.ps1、`ref_zh_long.txt`＝A/B 标准答案；**wav/mp3 不入库**，新机器用 progress.md 里的 TTS 脚本重建 `zh_test.wav` 与 `zh_long.wav`（45 s，mixed/重叠窗/A-B 用），`jfk.wav` 可从 `third_party\whisper.cpp\samples\jfk.wav` 复制）
A/B 输出：ab\（每配置一份 txt + `repo\` 对照，已 .gitignore）
自动化验证覆盖：质量下拉（CB_SETCURSEL=1 后日志出现「高精度模式下建议改用 large-v3」提示）、VAD/GPU 复选框（BM_CLICK）、GPU/CUDA 后端加载。**未自动化**：「选择术语表...」的文件对话框（其与 CLI `--terms` 共用 `load_terms_file()`，已用 CLI 验证解析/替换/提示词效果）。
未清理：build_w\（残留，删除时被占用，可忽略/手动删）
```

## 换机器（新环境）恢复步骤

```powershell
# 前置：VS2022 (Desktop C++)、CMake >= 3.16、git；目标机 Win10/11 x64
git clone <本仓库> ; cd <仓库目录>
powershell scripts\setup.ps1      # 拉 whisper.cpp
powershell scripts\build.ps1      # 构建并产出 dist\
# 首次运行会自动下载 1.51 GB 模型；离线场景：手动把
#   ggml-large-v3-turbo.bin 放进 %LOCALAPPDATA%\WhisperTxt\models\
# 来源：https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo.bin
#      国内镜像 https://hf-mirror.com/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo.bin
powershell -NoProfile -ExecutionPolicy Bypass -File test\gui_smoke.ps1 `
  -Exe "$PWD\dist\whisper2txt.exe" -Audio "$PWD\test\zh_test.wav"   # 端到端确认
```

若新机器有 NVIDIA GPU：`powershell scripts\build.ps1 -Gpu`（需 CUDA Toolkit ≥12.8，Blackwell 卡如 RTX 50 系必须 12.8+）；AMD GPU：`-Vk`（需 Vulkan SDK）；两者可同时开，运行期由 ggml 自动选设备。UI 有「使用 GPU 加速」复选框（默认勾选，取消即用 CPU），CLI 对应 `--gpu` / `--cpu`（默认 GPU）。

## 下一步任务（按优先级）

| 优先级 | 任务 | 说明 |
|---|---|---|
| 高 | **真实录音 A/B 标定** | 用用户自己的 10–60 min 真实 MP3（口音重/音质差）跑 `scripts\ab_quality.ps1`：定出预处理开/关、high/fast、是否换 large-v3、术语表内容、VAD 开/关。**当前调优数字只在 TTS 样本（含衰减 28 dB 变体）上得到**，不能当结论 |
| 高 | 真实长 MP3 压力测试 | 验证内存、边界质量、是否需要自动分段与 `duration_ms` 分块 |
| 高 | git 首次提交 + 分阶段 commit | 用户未要求前不要 push；只 commit 本地 |
| 中 | VAD 覆盖度阈值标定 | 现在硬编码 `coverage >= 85%` 走整段识别（`src/engine.cpp` 的 transcribe VAD 分支）；真实录音里多少占比以上该整段、多少该切块，需要 A/B 数据支撑，可能要挪到 CLI/UI |
| 中 | 预处理/术语表参数暴露到 UI | 现在 UI 只有「音频预处理」总开关（高通固定 100 Hz，见 `read_settings`）+ 术语表按钮；高通 Hz、增益目标、VAD 阈值/填充、`--no-speech-thold` 只有 CLI |
| 中 | 设置持久化 | 目前每次启动回到默认值；可写 `%LOCALAPPDATA%\WhisperTxt\settings.ini`（记住模型槽、语言、质量、复选框、术语表路径） |
| 中 | 可选降噪（谱减/RNNoise） | 默认**别开**——过度降噪会显著降识别率；实现后必须 A/B |
| 中 | 版本信息（VERSIONINFO 资源） | 便于用户识别版本；图标已做（`src/app.rc` + `src/app.ico`，生成脚本 `scripts\gen_icon.ps1`），还缺版本号 |
| 低 | 崩溃日志落盘 | 把 UI 日志同时写 `%LOCALAPPDATA%\WhisperTxt\logs\` |
| 低 | 拖放整个文件夹递归添加、`--threads` 上限按物理核校验 | 小改进 |

## 绝对不能做的事（避免回退）

- ❌ 关掉 `GGML_OPENMP`（会变慢 28×，见 progress.md 反例 #6）。
- ❌ 删 `dist\vcomp140.dll`（exe 会启动失败于未装 VC 运行库的机器）。
- ❌ 把模型 URL 改成 `ggml-org/whisper.cpp`（401）。正确命名空间：模型 `ggerganov/whisper.cpp`，VAD `ggml-org/whisper-vad`。
- ❌ 用 `params.detect_language = true` 做逐窗检测（会让 `whisper_full` 只检测语言、返回 0、不产出文本）。
- ❌ 去掉 `/utf-8` 编译选项（中文字面量会变乱码）。
- ❌ 改 `third_party/whisper.cpp` 内代码（保持 upstream 干净，需要补丁时在顶层 CMakeLists 或 src/ 里做）。

## 关键代码索引

| 位置 | 内容 |
|---|---|
| `src/engine.cpp:95` | `Transcriber::load()`：`ggml_backend_load_all()`（必需）+ GPU/CPU 选择（`cparams.use_gpu`、GPU 上 `flash_attn`）+ 设备日志 + `:162` VAD 上下文 `whisper_vad_init_from_file_with_params()`（失败只警告不报错） |
| `src/engine.cpp:36` | `gpu_device_list()`：枚举已加载后端里的 GPU 设备名（用于日志与「无 GPU 自动回落」提示） |
| `src/engine.cpp:280` | `Transcriber::transcribe()`：质量预设（fast greedy / high beam5 + 阈值）、`initial_prompt`、`apply_language()`/`build_window_prompt()` 两个 lambda、**VAD 切块分支（覆盖度 ≥85% 走整段）**、mixed 28/30 重叠窗、非 mixed 单调用；末尾去重 + 术语替换 |
| `src/engine.cpp:200` | `emit_segments`：分段抽取 + 厘秒→ms + `window_base_ms_` 偏移 + mixed 所有权 horizon（`window_own_limit_ms_`）+ 过滤空段/`[BLANK_AUDIO]` |
| `src/engine.cpp:716` / `:760` | `apply_terms()`（`错=>对` 替换，英文按词边界） / `dedupe_segments()`（归一化文本后的重复与前缀包含合并） |
| `src/engine.cpp:803` / `:837` | `make_vad_params()`（预设 + `VadTuning` 覆盖） / `vad_chunks()`（`whisper_vad_segments_from_samples` 的**厘秒**×10 → 合并 ≤28 s 块、间隔 ≤1 s；打日志「VAD 原始语音段: N 段 -> 合并为 M 块」） |
| `src/engine.cpp:58` | `physical_core_count()`：CPUID 判超线程 → 默认线程数 |
| `src/audio.cpp:81` | `decode_audio_16k_mono(...)`：`channels=0` 保持原声道 → 声道能量统计/取较响声道 → `highpass_biquad()`(:47) → RMS 响度归一化（目标 −20 dBFS，极弱音回退峰值归一化）+ 峰值限幅；输出 `AudioStats`（含 rms 前后/是否 loudnorm） |
| `src/main.cpp:164` | `ensure_transcriber_ctx()`：signature（含 quality/promptHash/termsCount/vad 调参）+ `EngineConfig` 填充 |
| `src/main.cpp:220` | `run_files()`：与 UI 无关的核心批处理流程（GUI 与 CLI 共用），含预处理统计日志 |
| `src/main.cpp:418` | `load_terms_file()`：术语表解析（注释、`A=>B` 替换、拼 `initial_prompt` ≤420 B） |
| `src/main.cpp:695` | `on_select_terms()`：术语表文件对话框 + 按钮回显 + 加载计数日志 |
| `src/main.cpp:979` | `run_cli_mode()`：CLI 参数解析与 console UTF-8 输出（`console_write()` :972） |
| `src/main.cpp` `WM_CREATE` / `wnd_proc` | 控件初始化、`WM_APP_*` 消息处理、关闭时 cancel+join |
| `src/downloader.cpp` `download_from_url` | Range 续传、416/206/200 分支、15 s 慢速→外层切镜像 |
| `src/models.cpp` `kModels/kVad/init_urls` | 模型 label/文件名/URL/min_size/expected_size |
| `src/ui.cpp` `ui_create` / `ui_layout` | 控件创建（id：101-105 按钮 / 201-204 下拉 / 301-305 复选 / 401-402 起停 / 501 列表 / 601 进度 / 701 日志）与坐标自适应；**每个控件都必须在 `ui_layout()` 里 MoveWindow**（改界面先看这里；最小窗口 880×520） |
| `scripts/ab_quality.ps1` + `tools/wer.py` | 多配置 A/B 跑批 + 编辑距离评分（`ERROR_RATE:` 行为解析锚点） |
| `CMakeLists.txt` | 构建选项集中处（OpenMP、静态 CRT、manifest、/utf-8、`WHISPER2TXT_SHARED`） |

## 已知环境问题（本机）

- HuggingFace 直连速度慢且偶发 TLS 中断（`schannel: server closed abruptly`）→ 程序会自动切 `hf-mirror.com`；手动下载也建议用镜像。
- `cmake --build` 报 `LNK1104 无法打开 whisper2txt.exe` = 程序还在运行，先 `Stop-Process -Name whisper2txt`。
- PowerShell 5.1 控制台显示中文日志会乱码（写文件时是正确 UTF-8），验证内容请用 `Get-Content -Encoding UTF8` 或直接读输出文件。
