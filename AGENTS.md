# AGENTS.md — 本项目的 AI 协作规范

面向接手本项目的 AI 编码助手（含换模型后接手）。人类负责人是需求与验收的最终决定者。

## 接手时必做

1. 读 `HANDOFF.md`（现状 + 禁止事项 + 下一步优先级）。
2. 读 `techdesign.md` §9「已知坑清单」。
3. 跑一次最小验证再动手改代码（**必须跑 `dist\` 里的 exe**，build 目录里 exe 与 DLL 分离会加载失败）：
   ```powershell
   powershell scripts\build.ps1 -Gpu -Vk
   dist\whisper2txt.exe --cli --lang zh --overwrite test\zh_test.wav
   ```
4. 改代码前先看 `src/engine.cpp` 的 `transcribe()` 与 `src/main.cpp` 的 `run_files()`——核心逻辑集中在这两处。

## 项目硬约束

- C++17、纯 Win32 GUI、whisper.cpp **动态库分发**（`WHISPER2TXT_SHARED=ON`：whisper/ggml DLL + `GGML_BACKEND_DL` 后端模块）、GPU 已启用（CUDA+NVIDIA、Vulkan+AMD，无 GPU 自动回落 CPU）。
- **不要关闭 `GGML_OPENMP`**（实测关慢 28×）。分发时必须保留 `dist\vcomp140.dll`。
- 模型 URL 命名空间：模型 `ggerganov/whisper.cpp`，VAD 模型 `ggml-org/whisper-vad`。
- 不修改 `third_party/whisper.cpp/` 内任何文件；需要变更时改顶层 `CMakeLists.txt` 或 `src/`。
- 输出约定：输入 `a.mp3` → 同目录 `a.txt`（+ `a.srt`），UTF-8 with BOM，`\r\n`。
- 界面文案保持中文；源码含中文字面量，编译必须带 `/utf-8`（已配置，勿删）。

## 编码风格

- 大括号一律 **Allman 风格**（左大括号单独一行，函数/控制语句/struct 都是）。用根目录 `.clang-format` 统一格式化：`& "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\LLVM\bin\clang-format.exe" -i -style=file src\*.cpp src\*.h`（注意 PATH 上的 clang-format 是 6.0.0，太旧，用 VS2022 自带的 19.x）。
- 遵循现有文件划分：`main/ui/engine/audio/output/downloader/models`，不要在 UI 线程做耗时工作。
- UI 线程与 worker 只通过 `PostMessageW` + `WM_APP_*` 通信，跨线程数据用堆分配结构并在处理后 `delete`。
- 不加注释，除非用户明确要求或存在非直观的关键约束。
- 错误路径一律返回带中文说明的 `std::wstring& err`，不静默失败。
- Windows API 用宽字符版本；传给第三方库前经 `u8()` 转 UTF-8。

## 验证清单（每次改动后至少跑前 3 项）

1. `powershell scripts\build.ps1` 无 error/LNK 错误。
2. CLI 中文样本：`dist\whisper2txt.exe --cli --lang zh --overwrite test\zh_test.wav` → `test\zh_test.txt` 内容仍准确、速度在 ~1.3× 实时以上（GPU 发行包下应 ≥30×）。**出厂默认已是 质量=high + VAD + 预处理**，所以这条命令的日志里必须出现 `loading VAD model` 与 `VAD 原始语音段: N 段`；要测 CPU/快速档另加 `--cpu` / `--quality fast` / `--no-vad` / `--no-preproc`。
3. CLI 混合模式：`--lang mixed`（长样本用 `test\zh_long.wav`）不产生空输出、无明显重复段、结尾不丢。
4. GUI 自动化：`powershell -NoProfile -ExecutionPolicy Bypass -File test\gui_smoke.ps1 -Exe <exe> -Audio test\zh_test.wav` → 期望 `CLICKED_START` + `OUTPUT_CREATED`。
5. 若动了下载器：删掉缓存模型（`%LOCALAPPDATA%\WhisperTxt\models\`）重跑，确认续传/切镜像日志。
6. 若改了 CMake 链接选项：`dumpbin /dependents` 确认只依赖系统 DLL + `VCOMP140.DLL`。
7. 若动了**识别参数或音频预处理**：跑 A/B 而不是只看能不能跑通 ——
   `powershell -NoProfile -ExecutionPolicy Bypass -File scripts\ab_quality.ps1 -Exe dist\whisper2txt.exe -Audio test\zh_long.wav -Ref test\ref_zh_long.txt -Terms <terms.txt> -Out ab`
   对比 `错误率` 与 `速度倍`；**没有真实录音标准答案时，不要把 TTS 样本上的数字当结论**（见 techdesign §9 #18/#19）。
   另：勾选/传了 `--vad` 就必须确认日志里出现 `loading VAD model` 与 `VAD 原始语音段: N 段 -> 合并为 M 块`，否则 VAD 根本没生效（`params.vad` 对 `whisper_full_with_state` 无效，见 §9 #23）。纯静音样本应输出 **0 行**（曾是 1 行幻觉）。
8. 若改了 UI 控件：用 `EnumChildWindows`+`GetWindowRect` 脚本核对 880×520 / 900×560 / 放大三种尺寸无重叠（临时脚本不入库）。**不要同时开两个 GUI 实例跑自动化**（两个进程各自载 1.6 GB 模型会互相拖慢，导致 `OUTPUT_TIMEOUT` 假失败）。

## Shell 注意（Windows PowerShell 5.1）

- 不支持 `&&`，用 `; if ($?) { ... }` 或分步执行。
- 切目录用工具的 `workdir` 参数，不要 `Set-Location` 内联。
- 中文输出在控制台可能乱码：用 `Get-Content -Encoding UTF8` 或直接读文件验证。
- **入库的 `.ps1` 里不要写中文字面量**（PS 5.1 按 ANSI 读，会解析错误）；要中文用 `[char]0xXXXX` 拼接或从文件读，或把脚本存成 UTF-8 with BOM。跨语言传数据用 ASCII 锚点（例：`tools\wer.py` 打印 `ERROR_RATE: 1.31%`）。
- 构建/下载等长任务用 `Start-Process` 后台 + 轮询日志，避免超时被杀。

## Git 纪律

- 未经用户明确要求不 commit / 不 push。仓库当前 0 commit，`third_party/`、`build/`、`dist/`、`*.log` 已在 `.gitignore`。
- 不提交模型文件（1.5 GB 级）与测试音频。
