# 项目进度 (progress.md)

项目：`whisper2txt` — Windows 本地 MP3 → TXT/SRT 图形工具（whisper.cpp 方案）。
仓库：`D:\DiskE\_github\whisper2txt`（分支 `main`，已 push 到 origin；**2026-11-04 的「动态库分发 + GPU 开关」改动尚未 commit**；`third_party/`、`build*/`、`dist/` 不入库）。
最后更新：2026-11-03。

## 状态总览

| 阶段 | 状态 |
|---|---|
| 方案设计（选型/模型/打包路线） | ✅ 完成（选型：whisper.cpp + Win32 C++ + CPU-only + 首次自动下载模型 + .txt/.srt） |
| 工具链与构建（VS2022 MSVC + CMake） | ✅ 完成，静态链接 whisper/ggml |
| 核心功能（解码/转写/输出/取消/批量） | ✅ 完成并实测 |
| GUI（拖放、下拉、进度、日志、打开目录） | ✅ 完成并自动化端到端验证 |
| CLI 模式（同一 exe） | ✅ 完成（额外功能，便于批量与自动验收） |
| 模型自动下载（续传 + 镜像回退） | ✅ 完成并实测（含中断续传） |
| VAD 加速（Silero） | ✅ 完成并实测（1.30× → 1.51× 实时） |
| 中英混合语音支持 | ✅ 完成（逐 30 s 窗自动检测） |
| 打包分发 | ✅ `dist\whisper2txt.exe` + `dist\vcomp140.dll` |
| 真实用户 MP3 长音频压力测试 | ⚠️ 未做（仅用样本/TTS 合成） |

## 里程碑记录

1. **环境探测**：Python 3.12/3.13 可用、无 ffmpeg、无 g++/cl 于 PATH、VS2022 Professional + CMake 4.4 + winget/choco 可用、无 `nvidia-smi` → 决定 CPU-only 路线。机器：Xeon E5-2697 v4 ×2（36 逻辑线程）、96 GB RAM、Win10 Pro、C 盘 112 GB / E 盘 714 GB 空闲。
2. **选型确认（与用户确认）**：whisper.cpp + Win32 C++；模型 large-v3-turbo（备选 medium / large-v3）；首次运行自动下载到本地缓存；同时输出 `.txt` + `.srt`；语言下拉需支持中文 / 英文 / 中英混合。
3. **clone whisper.cpp** → `third_party/whisper.cpp`（`git clone --depth 1`，ggml 0.28.0，commit 4da27c4）。确认 examples 默认用 miniaudio 解码（内置 MP3）→ 无需 ffmpeg。
4. **工具链验证**：`whisper.lib + ggml.lib + ggml-cpu.lib` 静态构建成功；CMake 检测 `/arch:AVX2 + FMA + F16C`，AVX512 不可用（符合预期）。
5. **模型获取**：正确 URL 为 `https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo.bin`（1,624,555,275 B）。`ggml-org/whisper.cpp` 返回 401。curl 直连中途 TLS 断（`schannel: server closed abruptly`），后由程序自身 WinHTTP 续传完成（1,618,902,436 → 1,624,555,275）。
6. **代码实现**：`src/{main,ui,engine,audio,output,downloader,models}.cpp` + `app.manifest` + 顶层 `CMakeLists.txt`。
7. **编译错误修复**：`<windowsx.h>` 缺失、`CLEARTYPE_QUALITY` 拼写、`const left_w` 误赋值、`g_prefill_files` 声明顺序。
8. **OpenMP 事故与结论**：为去掉 `VCOMP140.DLL` 关闭 `GGML_OPENMP` → 中文样本 10.6 s 变 298.5 s（0.05× 实时）。恢复 ON；尝试 `/exportOpenMP` 无效 → 结论：分发 `vcomp140.dll`（212,920 B）。
9. **静态 CRT**：`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` → 去掉全部 `VCRUNTIME140*`/`api-ms-win-crt-*` 导入。
10. **端到端验收**（见下表）。

## 实测数据

| # | 样本 | 模式 | 音频 | 用时 | 速度 | 结果 |
|---|---|---|---|---|---|---|
| 1 | `test/jfk.wav` | turbo + en | 11 s | 9.7 s | 1.14× | 整句准确，1 段 |
| 2 | `test/cn_test.mp3`(实为 COC 游戏女声 "Thank you.") | turbo + en | 30 s | 12.7 s | 2.36× | MP3 解码正常，输出 `Thank you.` |
| 3 | `test/zh_test.wav`(TTS Huihui 中英混合) | turbo + zh | 15 s | 12.3 s | 1.30× | 2 段，中文准确，英文词保留 |
| 4 | 同上 | turbo + mixed | 15 s | 19.8 s | 0.81× | 与 zh 结果一致 |
| 5 | 同上 | turbo + zh + VAD | 15 s | 10.6 s | 1.51× | VAD 模型自动下载 885,098 B |
| 6 | 同上（关 OpenMP） | turbo + zh + VAD | 15 s | 298.5 s | 0.05× | 反例，已回退 |
| 7 | GUI 自动化点击（`test/gui_smoke.ps1`） | 默认 GUI 选项 | 15 s | 输出文件 20 s 内出现 | — | 通过 |
| — | 模型加载（首次） | turbo | — | 8.4–8.8 s | — | 批量复用同一实例 |

### GPU 化后实测（2026-11-04，新机器：RTX 5070 Ti 16 GB + CUDA 13.4 + Vulkan SDK 1.4.363）

| # | 样本 | 模式 | 音频 | 用时 | 速度 | 结果 |
|---|---|---|---|---|---|---|
| 8 | `test/zh_test.wav` | turbo + zh + **GPU(CUDA)** | 15 s | 0.3 s | 46.5× | 2 段，内容正确 |
| 9 | 同上 | turbo + zh + VAD + GPU | 15 s | 0.3 s | 49.4× | 同上 |
| 10 | 同上 | turbo + zh + **CPU**（取消勾选 / `--cpu`） | 15 s | 9.1 s | 1.75× | 与 GPU 结果一致 |
| 11 | 同上 | turbo + zh，GPU 勾选但删掉 `ggml-cuda.dll`+`ggml-vulkan.dll` | 15 s | — | CPU | 日志提示后自动回落 CPU，不崩 |
| 12 | 同上 | turbo + zh，改名掉 `ggml-cuda.dll` | 15 s | — | — | 自动选 `Vulkan0`，内容一致 |
| — | 模型加载 | GPU / CPU | — | 6.4 s / ~7 s | — | — |

界面复选框「使用 GPU 加速」（id 304，默认勾选）已验证：GUI 自动化取消勾选后走 CPU（11–14 s 出结果），勾选时 3 s 出结果；日志区显示所用设备。

`zh_test.wav` 转写结果（准确性证据）：

```
今天的Project会议定在下午三点,我们需要review一下这个Prototype的设计方案,确保整体的用户体验没有问题。
另外,请大家把各自的Report提前发到群里,方便沟通。
```

### 识别率调优后实测（2026-11-04 同日，RTX 5070 Ti + CUDA 13.4）

新增能力：音频预处理（较响声道 + 100 Hz 高通 + 峰值归一化）、`--quality fast|high`（high = beam5 + `temperature_inc 0.4` / `entropy_thold 1.5` / `logprob_thold −1.2` + VAD 调参；`no_speech_thold` 保持库默认 0.6，见 techdesign §9 #22）、术语表（`initial_prompt` + `错=>对` 后处理替换）、分段去重、mixed 28/30 重叠窗 + 跨窗 prompt 携带、GPU `flash_attn`。

| # | 样本 | 模式 | 音频 | 用时 | 速度 | 结果 |
|---|---|---|---|---|---|---|
| 13 | `zh_test.wav` | turbo + zh + 预处理 + flash_attn（现默认） | 15 s | 0.3 s | 58.8× | 内容正确，分段变细（2→5 段） |
| 14 | 同上 | turbo + zh + high + VAD | 15 s | 0.4 s | 38.0× | 内容正确 |
| 15 | 同上 | turbo + zh + CPU（预处理开） | 15 s | 9.1 s | 1.76× | 与 GPU 一致 |
| 16 | `zh_long.wav`（新建 45 s 长样本） | turbo + mixed，fast | 45 s | 0.6 s | 81.7× | 10 段，无重复段，结尾不丢 |
| 17 | 同上 | turbo + mixed，high + VAD | 45 s | 1.1 s | 46.9× | 同上 |
| 18 | 同上 | turbo + mixed，high + VAD + 术语表 | 45 s | 1.2 s | 48.7× | 专名纠正：`购语言`→`Go 语言`、`CodeReview`→`Code Review` |
| 19 | 同上 | 术语表后处理替换验证 | — | — | — | `Prototype=>原型`、`设计方案=>设计规划` 生效 |
| 20 | `zh_stereo.wav`（ch0=×0.2、ch1=原） | 预处理声道选择验证 | 15 s | 0.5 s | 33.4× | 日志「声道 2 取 ch1」，转写与单声道一致 |
| 21 | `silence30.wav`（30 s 纯静音） | **修 VAD 前** turbo + high | 30 s | 0.2 s | 173× | 幻觉 1 行 `中文字幕志愿者 杨栋梁` |
| 22 | 同上 | **修 VAD 后** turbo + high + VAD | 30 s | — | — | **0 行输出**（静音不再被强行识别） |
| 23 | `zh_padded.wav`（20 s 静音+45 s 语音+15 s 静音） | turbo + high + VAD + 术语表 | 80 s | — | — | VAD 覆盖 55% → 切 2 块、只送 44 s；SRT 首段 `00:00:19,980`（时间轴不漂）；错误率 1.31% |
| 24 | `zh_quiet.wav`（幅度 ×0.04 ≈ −28 dB，模拟差录音，`--no-preproc`） | turbo + high | 45 s | — | — | 3.27% |
| 25 | 同上 | **large-v3** + high | 45 s | 3.8 s | 11.9× | **2.61%** |
| 26 | 同上 | large-v3 + high + 术语表（含开预处理也一样） | 45 s | — | — | **0.65%** |
| 27 | `zh_long.wav` | large-v3 + high + 术语表 | 45 s | — | — | **0.65%**（turbo 同配置 1.31%） |

A/B 错误率（`scripts\ab_quality.ps1`，标准答案 `test\ref_zh_long.txt`，`tools\wer.py` 按字/词混合编辑距离；VAD 修好后重测）：

| 配置 | 速度 | 错误率 |
|---|---|---|
| fast（预处理开） | 79.1× | 3.27% |
| fast + `--no-preproc` | 82.3× | 3.27% |
| high | 41.8× | 3.27% |
| high + VAD（覆盖 ≥85% → 自动整段） | 40.2× | 3.27% |
| high + VAD + 术语表 | 42.3× | **1.31%** |
| large-v3 + high + 术语表 | — | **0.65%** |

剩余 2 处差异是 `三点/3点`、`十/10` 这类数字写法（非识别错误）。**注意**：样本是 TTS 合成语音（`zh_quiet` 是它衰减 28 dB 的变体），只能证明机制生效与不劣化；口音重/音质差的真实录音必须重新 A/B（见 techdesign §9 #18/#19/#22/#23）。

### 本轮复查修掉的两个真 bug（2026-11-04 晚）

1. **VAD 之前完全无效**：whisper 的 `params.vad` 只在 `whisper_full()` 内实现，本项目全用 `whisper_full_with_state()` → UI 勾选与 `--vad` 是空操作（日志里连 `loading VAD model` 都没有）。改为自实现：`whisper_vad_segments_from_samples()`（**返回厘秒**）→ 合并 ≤28 s 块 → 逐块 `with_state` 并按块起点偏移时间戳；再加「语音覆盖 ≥85% 就整段识别」的启发式（否则连续录音切窗会把 3.27% 变 4.58%）。验证见上表 #21–#23。
2. **`no_speech_thold=-0.6` 用错语义**：该字段在本版本是概率阈值（`no_speech_prob = probs[nospeech]`），负数等于「低置信窗一律丢整窗文本」→ 已从 high 预设移除，改回库默认 0.6，另加 `--no-speech-thold` 显式覆盖（见 techdesign §9 #22）。

预处理观察：同一 TTS 样本开启预处理后分段 6→13、`三点`→`3点`，错误率不变 —— 说明预处理会改变 whisper 的分段/书写选择，必须实测而非臆断。

### 程序图标（2026-11-06）

- 图标图样当前取仓库根 `speechtotext2.png`（512×512、白底**不透明**，主色 #D9F2F2 / #4D4D4D / #F3A766）；`scripts\gen_icon.ps1`（默认 `-Source speechtotext2.png`）生成 `src/app.ico`：16/24/32/48/64/128/256 七档 PNG 压缩帧，整文件 33 KB（`-Source` / `-Out` / `-Sizes` 可重跑）。首版用的是 `speechtotext.png`（700×700 透明底，12 KB），已被替换，旧图仍留在仓库根。
- `src/app.rc` + `src/resource.h`（`IDI_APPICON = 101`）作为 `add_executable` 的额外源文件编进 exe；`.rc` 里 `ICON "app.ico"` 能解析是因为 `target_include_directories(... src)` 把 `src` 传给了 rc.exe（techdesign §9 #24）。`main.cpp` 用 `LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON))` 设 `wc.hIcon`、`LoadImageW(..., SM_CXSMICON/CYSMICON)` 设 `wc.hIconSm`，取不到才回落 `IDI_APPLICATION`。
- 脚本不做抠图：新图白底不透明 → 图标是实心方块（无透明边角）。要透明得先在外部把白底抠掉，或给 `gen_icon.ps1` 加去背步骤。
- 验证（换图后重跑）：exe 861 KB，内嵌 `RT_GROUP_ICON` #101 有 7 帧（16/24/32/48/64/128/256，PNG 压缩）；`LoadImageW` 在 16/24/32/40/48/64/256 全部成功（PNG 帧可缩放）；`scripts\build.ps1 -Gpu -Vk` 全绿、CLI 中文样本正常输出、GUI 自动化 `CLICKED_START` + `OUTPUT_CREATED`（3 s）。

## 决策记录

- **不用 Python/faster-whisper/PyInstaller**：避免 150–400 MB exe、冷启动、ffmpeg 依赖。
- **不用 curl 外部进程下载**：改用 WinHTTP，进度可控、可取消、可续传、可切镜像。
- **输出 `歌.txt`（替换扩展名）**，而不是 `歌.mp3.txt`。
- **mixed 模式 = 28/30 s 重叠窗 + 每窗 auto-detect**，不使用 `params.detect_language`（会只检测不转写）；每窗尾部 2 s 交给下一窗（所有权 horizon 29 s）+ 携带上一窗尾部文本作 `initial_prompt`。
- **默认语言 = 中文(zh)**：对夹杂英文词的中文语音效果好且比 mixed 快约 2×。
- **音频预处理默认开**（取较响声道 + 100 Hz 高通 + 峰值归一化到 −1 dBFS，增益限幅 ±24 dB）；`--no-preproc` 一键关掉，日志打印实测增益便于 A/B。
- **默认质量档保持 fast(greedy, best_of=1)**：GPU 上 high 慢约 25–35% 但错误率在 TTS 样本上未见改善 → 把 high 作为可选项，真正的增益来自术语表与换 large-v3。
- **术语表两用途**：普通行进 `initial_prompt`（偏置专名/风格），`A=>B` 行走后处理替换（英文按词边界）；上限 ~420 B 防止吃掉解码上下文。
- **不做默认降噪**：过度降噪反而降识别率；要做得先 A/B。

## 遗留 / 待办

- 用真实长中文 MP3（≥10 min）验证：长音频单次 `whisper_full` 的内存与边界质量、是否需要 >2 h 自动分段。
- **真实录音上的调优标定**（当前调优参数只在干净 TTS 样本上验证）：用 `scripts\ab_quality.ps1` + 人工标准答案，定出预处理开关、fast/high、是否换 large-v3、术语表内容。
- 混合语音的窗边界丢词观察（重叠窗已实现，但 45 s 样本上未覆盖“切在词中间”的极端情况）。
- 未做：词级时间戳、说话人分离、翻译任务（turbo 不支持 translate）、可选降噪、UI 上的预处理/术语表细参数（高通 Hz、VAD 阈值）、设置持久化。
- 未做：安装器、版本信息资源（VERSIONINFO，当前无版本号）。图标已完成（`src/app.rc` / `src/app.ico` / `scripts\gen_icon.ps1`）。
- 未做：崩溃/异常日志落盘（目前仅 UI 日志区）。
- `build_w/` 残留目录删除时被进程占用，可稍后手动删（已被 .gitignore 忽略）。

## 快速复现验证

```powershell
powershell scripts\build.ps1 -Gpu -Vk      # 动态库 + CUDA + Vulkan（产物已自动汇总到 dist\）
dist\whisper2txt.exe --cli --lang zh --overwrite test\zh_test.wav
dist\whisper2txt.exe --cli --lang mixed --overwrite test\zh_test.wav
dist\whisper2txt.exe --cli --vad --overwrite test\zh_test.wav
dist\whisper2txt.exe --cli --cpu --overwrite test\zh_test.wav   # 强制 CPU
powershell -NoProfile -ExecutionPolicy Bypass -File test\gui_smoke.ps1 `
  -Exe "$PWD\dist\whisper2txt.exe" `
  -Audio "$PWD\test\zh_test.wav"
```

生成新的中文测试音频（需要系统带 zh-CN TTS，本机有 `Microsoft Huihui Desktop`）：

```powershell
Add-Type -AssemblyName System.Speech
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
$s.SelectVoice("Microsoft Huihui Desktop"); $s.Rate = 1
$s.SetOutputToWaveFile("$PWD\test\zh_test.wav")
$s.Speak("你的测试文本")
$s.Dispose()
```
