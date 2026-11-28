# IR SDK 跨平台设计说明

IR SDK 面向 Windows x64 与 Ubuntu 24.04 x86-64 使用同一份源码和公共 C 头文件，生成两个动态库。

| 库 | Windows | Linux | 职责 |
|---|---|---|---|
| Station | `IR_station.dll` | `libIR_station.so` | 仪器控制、原始帧采集、JSON 测试流程与结果保存 |
| Analysis | `IR_analysis.dll` | `libIR_analysis.so` | 帧均值、时域噪声、ROI 均值、Noise3D 两阶段、GB/GJB、闪元与 1D/2D FFT（零 Qt/OpenCV） |

应用包含 `IR_station.h`、`IR_device.h`、`IR_recipe.h`、`IR_analysis.h` 或 `IR_plugin_abi.h`，以 `IR_*` 调用 C 接口。两个库分别提供 `IR_StationVersion()` 与 `IR_AnalysisVersion()` 运行期版本查询；Windows DLL 同时内嵌 `VERSIONINFO` 文件版本资源。公共状态码与 UTF-8 错误缓冲区约定见 `IR_status.h`。客户程序只需要两个 SDK 库及所用语言的运行环境；详细接入与配方/样例说明见 [客户接入说明](docs/客户接入说明.md)。

## 客户集成入口

| 入口 | 用法 | 说明 |
|---|---|---|
| C ABI | `include/IR_*.h` + `windows-x64/{bin,lib}`（或 `linux-x86_64/lib`） | 纯 C99/C11 头文件，零第三方运行库 |
| CMake 包 | `find_package(IRSDK CONFIG REQUIRED)` → `IRSDK::IR_station`、`IRSDK::IR_analysis` | 标准安装前缀走 `<prefix>/lib/cmake/IRSDK/`；解压包走 `<pkg_root>/cmake/IRSDKConfig.cmake`（`-DIRSDK_DIR=<pkg_root>/cmake`）。同时提供 `IRSDK_VERSION`、`IRSDK_ROOT_DIR`、`IRSDK_INCLUDE_DIR`、`IRSDK_PYTHON_BINDINGS_DIR` |
| Python | `import irsdk`（`bindings/python/irsdk`） | 纯 `ctypes` 镜像：枚举与状态码常量、41 个公共结构体尺寸校验、57 个导出符号签名、`load_sdk`/`call_sdk`/`sdk_versions` |
| CLI | `windows-x64/bin/ir_run_recipe.exe`、`ir_capture_accept.exe` | 离线预检、配方执行、采帧拷机 |

包内目录形状：`include/`、`windows-x64/bin`、`windows-x64/lib`、`linux-x86_64/lib`、`cmake/`、`bindings/python/`、`examples/`、`simulators/`、`docs/`、`third_party/`、`SDK.md`。`third_party/nlohmann_json` 仅供独立编译的 CLI 样例使用，不进入 SDK 双库的运行时依赖。

## 四层能力入口

| 层 | 核心入口 | 能力说明 |
|---|---|---|
| 仪器控制 | `IR_OpenDevice`、`IR_ListSerialPorts` | 黑体（PCN7-LT TCP / DCN1000 串口，可选 `min_setpoint_c`/`max_setpoint_c` 软件设定窗口，默认 -60..100 / -5..100）、IT6332A 电源（TCP / 串口 / Windows USB/NI-VISA，`IR_WaitPowerCurrentStable` 启动冲击确认与稳态电流等待）、GWS STEN 烘箱（`IR_SetOven`/`IR_ReadOven`/`IR_StopOven`/`IR_WaitOvenStable`）、Keithley DMM6500 SCPI（TCP、串口、Windows USB/NI-VISA；`IR_SetDmmMode`/`IR_ReadDmm`）、探测器 `0xCC` 串口（`IR_SetDetectorTiming`/`IR_SetDetectorGpol`/`IR_SetDetectorRegister`/`IR_MeasureDetectorCurrent`）与制冷机 Modbus RTU（`IR_COOLER_MODBUS`，默认 `8N2`） |
| 原始采集 | `IR_ListCaptureDevices`、`IR_OpenFrames`、`IR_ReadFrames` | RAW16 / RAW32_LE 离线回放（`IR_RAW_REPLAY`）、U16 DirectShow（`IR_DIRECTSHOW`）、U16 V4L2（`IR_V4L2`）；`IR_FlushFrames` 支撑不停流多 Burst 采集；`IR_TryReadLatestFrame` 提供无阻塞预览，`IR_GetCaptureStats` 提供线程安全的有界 `FrameQueue` 诊断（可能短暂等待预览复制） |
| 指标计算 | `IR_analysis` 独立动态库 | `IR_FrameStatistics`、`IR_ROIMean`、`IR_FrameStatisticsNuc`、Noise3D 帧计算、`IR_ChannelSpectrum` 与 `IR_FlickerDetect` 通过 `IRArrayView` 接收 U16/U32 紧密帧数组；GB/GJB、两点 NUC、`15×15` 盒式/高斯空间滤波、算术扣噪/阈值掩码、1D/2D FFT（`IR_Fft2dMagnitudeF64`）与 `IR_GjbEvaluateBurstMetrics` |
| 流程编排 | `IR_CheckRecipeJson`、`IR_RunRecipeJson` | 统一 `schemav` 流程格式（多温点/多条件、跨步像元图与掩码引用、`flush_frames`、可选 `save_raw`、`repeat_burst` 周期无存盘温漂、16 项内置算子、`IR_plugin_abi.h` 外部算子插件、`step_records` 逐步耗时及 `safety.blackbody_standby`/`safety.oven_standby` 待机收尾） |

## 线程与设备占用

一个进程同时打开一个采集源。打开、启动、读取（`IR_ReadFrames`）、冲刷（`IR_FlushFrames`）、停止、关闭和同步流程由同一工作线程调用，并发操作采集流返回 `IR_BUSY`；实时源由有界 `FrameQueue`（默认 64 帧，可通过 `IR_SetFrameQueueCapacity` 调整）在后台泵线程持续拉帧。UI 或监控线程可调用 `IR_TryReadLatestFrame` 尝试获取最新帧，该调用可能在预览复制期间返回暂无帧；`IR_GetCaptureStats` 线程安全，但可能短暂等待预览复制。离线 `IR_analysis` 调用可在其他线程使用各自输出缓冲区并行执行。

`IR_RunRecipeJson` 为 C / C# / Python 提供统一的 UTF-8 工位与流程 JSON 入口，采用 `schemav` 格式（`schemav` 默认 `1`，可省略）。`IR_CheckRecipeJson` 校验工位和配方，并加载工位配置中的插件。运行入口先按实际画幅核对批次容量与 ROI，准备成功后才打开并预约配方引用的仪器。本次运行可独占并写入输出目录时，执行失败或取消也会生成诊断版 `result.json`（含 `run_status`、`error_status`、`failed_stage_name`、`step_records` 与 `total_duration_ms`）。`IR_BUSY` 仅返回状态。

多工位部署推荐一工位一进程，便于独立启停和隔离驱动故障。黑体、温箱和电源的独立等待与配方、已启动的采集共用进程级取消状态。其他线程可调用 `IR_RequestStop` 取消当前活动操作。仪器传输和重连等待支持提前结束，待机与下电由原工作线程按设备通信超时执行并记录状态。

## 支持边界与标准差口径

| 能力 | Windows x64 | Linux x86_64（源码级） | 说明 |
|---|---|---|---|
| RAW16 / RAW32_LE 回放与 U16 实时采集 | 已验证（回放/队列/预览）；DirectShow 实机待验 | 源码支持（RAW16 / RAW32_LE 回放、U16 V4L2） | RAW32_LE 仅用于离线小端 U32 存储；不代表硬件支持某种 20-bit 传输编码；不做 RGB/YUV 转换 |
| 仪器控制与虚拟工位 | TCP 虚拟工位与 DMM6500 USB/NI-VISA 真机二极管档读取已验证；IT6332A USB/NI-VISA 已实现协议与资源互斥并在离线校验/TCP 回归中通过（真机待验）；6 路 `com0com` HIL 为可选验收项 | 源码支持（Asio TCP/串口/PTY） | 覆盖 PCN7-LT/DCN1000/IT6332A/GWS STEN/DMM6500/0xCC 探测器/Modbus 制冷机 |
| PCIe-1433 NI-IMAQ | 暂不支持（返回 `IR_UNSUPPORTED`） | 暂不支持 | 枚举编号 `IR_NI_IMAQ = 4` 已预留 |
| 内置分析算子与外部插件 | 离线与小数组全通过（`<= 1e-8 DN`） | 源码支持 | 零 Qt/OpenCV；`IR_plugin_abi.h` 支持 `plugin:<ns>.<name>` 动态插件 |

- **标准差口径**：JSON 配方 `temporal_noise` 输出的 `temporal_noise_dn` 采用逐像元**样本标准差**（`sample_std = 1`，分母 `N - 1`）；GB/GJB 噪声图 `vn` 采用**总体标准差**（`sample_std = 0`，分母 `N`）。
- **高位宽物理指标**：U32 通用图像算子可处理超过 65535 的 DN；当前 GB/GJB 配方指标仍使用现有物理量模型，因此 `valid_bits > 16` 或 U32 位宽未知时会在预检阶段拒绝执行。不得由 `valid_bits` 自动推导 `digital_full`、ADC 满量程或探测器饱和点。
- **参考 RAW16 复算误差**：`TC002A (640×512, 100帧)` 与 NumPy 最大误差 `6.86e-13 DN`；`S07-H1A (1280×1024, 1000帧)` 最大误差 `2.08e-11 DN`。
- **Noise3D 输入限制**：当前单线程执行，`thread_count` 为保留字段。单次调用上限为每帧 1000 万像元、每批 10 亿样本，按输入数组的 `frames × height × width` 计数。`1280×1024×1000` 超过此算子的单批上限。

## 第三方组件与运行环境

| 组件 | 版本 | 用途 | 客户运行时是否需要 | 源码与许可证 |
|---|---:|---|---|---|
| standalone Asio | 1.36.0 | `IR_station` 内部 TCP 与串口 I/O | 否，静态编入 | Boost Software License 1.0（客户包不携带源码） |
| nlohmann/json | 3.12.0 | 工位/配方 CLI 与样例解析 | 否，头文件编入 | MIT 许可证随样例包提供 |

- **Windows x64**：需要 MSVC x64 运行库（Visual C++ Redistributable），公共头文件与字符串参数统一为 UTF-8。
- **Linux x86_64**：源码级可移植（C++20 / CMake 3.21+ / Ninja），运行时仅依赖系统 C/C++ 运行库（GLIBC 2.38+），零 Qt/OpenCV 依赖。
