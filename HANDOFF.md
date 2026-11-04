# 交接说明 (HANDOFF.md)

用途：换 AI 模型 / 换机器时，让接手者 **10 分钟内恢复上下文并继续**。

## 一句话现状

功能已全部可用的阶段：`whisper2txt`（C++17 + 纯 Win32 + whisper.cpp large-v3-turbo）已完成 GUI + CLI + MP3 解码 + 同名 `.txt`/`.srt` 输出 + 模型自动下载续传/镜像回退 + 取消 + 批量 + VAD 加速，并已端到端实测通过。**2026-11-04 起改为动态库分发**：`whisper.dll/ggml.dll/ggml-base.dll` + 运行时按需加载的后端 DLL（`ggml-cuda.dll` / `ggml-vulkan.dll` / 9 个 `ggml-cpu-*.dll`），一次构建同时支持 NVIDIA(CUDA 13.4) 与 AMD(Vulkan)，`whisper2txt.exe` 仅 ~750 KB；`use_gpu=true`（无 GPU 自动回落 CPU），CUDA0 实测优先、缺 cuda DLL 自动落 Vulkan0。源码已 commit 到 `main`（dist/ 内 CUDA 后端产物为新改动，未 commit）。

## 接手必读顺序

1. 本文件
2. `techdesign.md` §9「已知坑清单」（8 条，全部会踩）
3. `progress.md` 实测数据表 + 遗留清单
4. 需要改代码时：`src/engine.cpp`（whisper 调用细节）、`src/main.cpp`（GUI/CLI/线程/消息）

## 当前工作区状态

```
已生成产物（动态库方案，2026-11-04，build_gpu\）：
  dist\whisper2txt.exe          769,536 B   （链接 whisper.dll，不再内嵌后端）
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
测试：test\  （样本 wav/mp3、输出 txt/srt、out_*.log、gui_smoke.ps1；**wav/mp3 不入库**，新机器用 progress.md 里的 TTS 脚本重建 `zh_test.wav`，`jfk.wav` 可从 `third_party\whisper.cpp\samples\jfk.wav` 复制）
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
| 高 | 真实长 MP3 压力测试 | 找 10–60 min 真实中文 MP3，验证内存、边界质量、是否需要自动分段与 `duration_ms` 分块 |
| 高 | git 首次提交 + 分阶段 commit | 用户未要求前不要 push；只 commit 本地 |
| 中 | mixed 模式窗边界质量改进 | 现方案 30 s 无重叠切窗；可试 28/30 s 重叠 + 去重，或按 VAD 段边界切窗 |
| 中 | VAD 参数暴露到 UI | `whisper_vad_default_params()` 的 threshold / min-speech-duration 等做 UI 微调 |
| 中 | 版本信息与图标（.rc） | 便于用户识别版本；注意 rc.exe 与 CMake 集成 |
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
| `src/engine.cpp:75` | `Transcriber::load()`：`ggml_backend_load_all()`（必需）+ GPU/CPU 选择（`cparams.use_gpu = cfg_.use_gpu`）+ 设备日志 |
| `src/engine.cpp` `gpu_device_list()` | 枚举已加载后端里的 GPU 设备名（用于日志与「无 GPU 自动回落」提示） |
| `src/engine.cpp:204` | `Transcriber::transcribe()`：非 mixed 单调用 / mixed 逐 30 s 窗循环（mixed 分支从 :252 起） |
| `src/engine.cpp` `emit_segments` | 分段抽取 + 厘秒→ms + `window_base_ms_` 偏移 + 过滤空段/`[BLANK_AUDIO]` |
| `src/engine.cpp` `physical_core_count()` | CPUID 判超线程 → 默认线程数 |
| `src/main.cpp` `run_files()` | 与 UI 无关的核心批处理流程（GUI 与 CLI 共用） |
| `src/main.cpp` `JobContext` | 解耦 UI/CLI 的 log+progress+cancel 注入结构 |
| `src/main.cpp` `run_cli_mode()` | CLI 参数解析与 console UTF-8 输出 |
| `src/main.cpp` `WM_CREATE` / `wnd_proc` | 控件初始化、`WM_APP_*` 消息处理、关闭时 cancel+join |
| `src/downloader.cpp` `download_from_url` | Range 续传、416/206/200 分支、15 s 慢速→外层切镜像 |
| `src/models.cpp` `kModels/kVad/init_urls` | 模型 label/文件名/URL/min_size/expected_size |
| `src/ui.cpp` `ui_create` / `ui_layout` | 控件创建（含 id：101-104 按钮 / 201-203 下拉 / 301-304 复选 / 501 列表 / 601 进度 / 701 日志）与坐标自适应；**每个控件都必须在 `ui_layout()` 里 MoveWindow**（改界面先看这里） |
| `CMakeLists.txt` | 构建选项集中处（OpenMP、静态 CRT、manifest、/utf-8） |

## 已知环境问题（本机）

- HuggingFace 直连速度慢且偶发 TLS 中断（`schannel: server closed abruptly`）→ 程序会自动切 `hf-mirror.com`；手动下载也建议用镜像。
- `cmake --build` 报 `LNK1104 无法打开 whisper2txt.exe` = 程序还在运行，先 `Stop-Process -Name whisper2txt`。
- PowerShell 5.1 控制台显示中文日志会乱码（写文件时是正确 UTF-8），验证内容请用 `Get-Content -Encoding UTF8` 或直接读输出文件。
