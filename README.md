# Whisper2Txt — 本地 MP3 转文字工具 (Windows)

纯本地运行的 MP3 → 文本工具：基于 [whisper.cpp](https://github.com/ggml-org/whisper.cpp) 的 C++/Win32 单程序，带简单图形界面，选择 MP3 后在同目录生成同名 `.txt`（可同时生成 `.srt`）。全程不上传网络（仅模型文件首次需联网下载）。

## 产物

```
dist\whisper2txt.exe     ~2.5 MB   主程序（GUI + CLI 同一 exe）
dist\vcomp140.dll        ~208 KB   OpenMP 运行时，必须与 exe 同目录
```

模型缓存（不在仓库内）：

```
%LOCALAPPDATA%\WhisperTxt\models\
  ggml-large-v3-turbo.bin    1,624,555,275 B   默认模型
  ggml-silero-v6.2.0.bin         885,098 B     可选 VAD 加速模型
```

## 使用（GUI）

1. 双击 `dist\whisper2txt.exe`
2. 拖放 MP3 到窗口，或点「添加 MP3 文件...」（支持多选，也接受 `.wav` `.flac`）
3. 选择：
   - 模型：`large-v3-turbo`（默认）/ `medium` / `large-v3`
   - 语言：`中文 (zh)`（默认）/ `英文 (en)` / `自动检测` / `中英混合 (逐 30 秒窗自动检测)`
   - 线程：默认「自动」= 物理核心数
   - 勾选：同时生成 `.srt`、启用 VAD 加速、覆盖已有输出
4. 点「开始转写」→ 进度条 + 日志 → 完成后点「打开输出目录」

模型缺失时点开始会自动下载到上述缓存目录（HuggingFace 直连慢时自动切 `hf-mirror.com`，支持断点续传，src/downloader.cpp:313）。

## 使用（CLI，同一 exe）

```powershell
dist\whisper2txt.exe --cli test\a.mp3 test\b.mp3
  --model turbo|medium|large    默认 turbo
  --lang zh|en|auto|mixed       默认 zh；mixed = 每 30 秒窗自动检测语言
  --threads N                   默认物理核心数
  --vad                         启用 Silero VAD 跳过静音（需下载 VAD 模型）
  --overwrite                   覆盖已存在的输出
  --no-srt                      只写 .txt
```

把 MP3 直接拖到 `whisper2txt.exe` 图标上也会走 CLI 模式（有参数即 CLI，无参数即 GUI）。加 `--gui 文件.mp3` 可预填 GUI 文件列表。

## 从源码构建

前置：Windows 10/11 x64、Visual Studio 2022（MSVC）、CMake ≥ 3.16、git。

```powershell
powershell scripts\setup.ps1     # clone whisper.cpp 到 third_party（若缺失）
powershell scripts\build.ps1     # configure + build + 拷贝到 dist\
```

## 性能与验证结果（本机 Xeon E5-2697 v4 ×2, 36 线程, 96 GB, CPU-only, AVX2）

| 样本 | 模型/模式 | 音频时长 | 用时 | 速度 |
|---|---|---|---|---|
| jfk.wav | turbo, zh→en | 11 s | 9.7 s | 1.14× 实时 |
| cn_test.mp3 | turbo, en | 30 s | 12.7 s | 2.36× 实时 |
| zh_test.wav | turbo, zh | 15 s | 12.3 s | 1.30× 实时 |
| zh_test.wav | turbo, zh + VAD | 15 s | 10.6 s | 1.51× 实时 |
| zh_test.wav | turbo, mixed（逐窗检测） | 15 s | 19.8 s | 0.81× 实时 |

模型加载约 8.4–8.8 s（一次性，批量转写复用同一实例）。

## 关键约束

- **必须开启 OpenMP**（`GGML_OPENMP=ON`）。关掉后 whisper 退化到极低并行度：同一中文样本从 10.6 s 变成 298.5 s（28× 慢）。因此 `vcomp140.dll` 必须随 exe 分发。
- 无 ffmpeg：MP3 由内置 miniaudio 解码（`third_party/whisper.cpp/examples/miniaudio.h`，调用见 `src/audio.cpp:14`）。
- 无 Python、无 CUDA。若要 GPU 需自行用 `-DGGML_CUDA=1` / `-DGGML_VULKAN=1` 重新构建 whisper 子项目。

## 仓库结构

```
CMakeLists.txt          顶层构建（静态链接 whisper/ggml）
scripts/setup.ps1       clone whisper.cpp
scripts/build.ps1       构建 + 打包到 dist\
src/main.cpp            WinMain、GUI/CLI 分流、任务线程、消息协议
src/ui.cpp|ui.h         纯 Win32 控件与布局、拖放
src/engine.cpp|engine.h whisper 模型加载、转写、进度/分段回调、取消、逐窗语言检测
src/audio.cpp|audio.h   miniaudio 解码 → 16 kHz 单声道 float32
src/output.cpp|output.h 写 .txt / .srt（UTF-8 BOM）
src/downloader.cpp      WinHTTP 下载、断点续传、镜像回退、慢速切换
src/models.cpp|models.h 模型元数据与缓存目录
src/app.manifest        asInvoker + PerMonitorV2 DPI + Common Controls v6
test/                   测试样本、日志、gui_smoke.ps1（UI 自动化）
docs → techdesign.md / progress.md / HANDOFF.md / AGENTS.md（根目录）
```

详细文档：`techdesign.md`（架构与踩坑）、`progress.md`（里程碑与实测）、`HANDOFF.md`（接续清单）、`AGENTS.md`（AI 协作规范）。
