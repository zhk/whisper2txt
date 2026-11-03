# 项目进度 (progress.md)

项目：`whisper2txt` — Windows 本地 MP3 → TXT/SRT 图形工具（whisper.cpp 方案）。
仓库：`E:\ProjectsMine\_gitHub_Debian9\Whisper`（分支 `master`，**尚无 commit**，全部为未跟踪文件）。
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

`zh_test.wav` 转写结果（准确性证据）：

```
今天的Project会议定在下午三点,我们需要review一下这个Prototype的设计方案,确保整体的用户体验没有问题。
另外,请大家把各自的Report提前发到群里,方便沟通。
```

## 决策记录

- **不用 Python/faster-whisper/PyInstaller**：避免 150–400 MB exe、冷启动、ffmpeg 依赖。
- **不用 curl 外部进程下载**：改用 WinHTTP，进度可控、可取消、可续传、可切镜像。
- **输出 `歌.txt`（替换扩展名）**，而不是 `歌.mp3.txt`。
- **mixed 模式 = 30 s 窗 + 每窗 auto-detect**，不使用 `params.detect_language`（会只检测不转写）。
- **默认语言 = 中文(zh)**：对夹杂英文词的中文语音效果好且比 mixed 快约 2×。

## 遗留 / 待办

- 用真实长中文 MP3（≥10 min）验证：长音频单次 `whisper_full` 的内存与边界质量、是否需要 >2 h 自动分段。
- 混合语音的窗边界丢词观察（目前仅 15 s 样本）。
- 未做：静音/长静音 VAD 参数调优、词级时间戳、说话人分离、翻译任务（turbo 不支持 translate）。
- 未做：安装器/图标/版本信息资源（`.rc`），当前无版本号。
- 未做：崩溃/异常日志落盘（目前仅 UI 日志区）。
- `build_w/` 残留目录删除时被进程占用，可稍后手动删（已被 .gitignore 忽略）。

## 快速复现验证

```powershell
powershell scripts\build.ps1
build\Release\whisper2txt.exe --cli --lang zh --overwrite test\zh_test.wav
build\Release\whisper2txt.exe --cli --lang mixed --overwrite test\zh_test.wav
build\Release\whisper2txt.exe --cli --vad --overwrite test\zh_test.wav
powershell -NoProfile -ExecutionPolicy Bypass -File test\gui_smoke.ps1 `
  -Exe "E:\ProjectsMine\_gitHub_Debian9\Whisper\dist\whisper2txt.exe" `
  -Audio "E:\ProjectsMine\_gitHub_Debian9\Whisper\test\zh_test.wav"
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
