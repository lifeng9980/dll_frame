# IR SDK 跨平台设计说明

IR SDK 面向 Windows x64 与 Ubuntu 24.04 x86-64 使用同一份源码和公共 C 头文件，生成两个动态库。

| 库 | Windows | Linux | 职责 |
|---|---|---|---|
| Station | `IR_station.dll` | `libIR_station.so` | 仪器控制、原始帧采集、JSON 测试流程与结果保存 |
| Analysis | `IR_analysis.dll` | `libIR_analysis.so` | 帧均值、时域噪声、ROI 均值、Noise3D 两阶段与 GB/GJB 核心评价（零 Qt/OpenCV） |

应用包含 `IR_station.h`、`IR_device.h`、`IR_recipe.h`、`IR_analysis.h` 或 `IR_plugin_abi.h`，以 `IR_*` 调用 C 接口。两个库分别提供 `IR_StationVersion()` 与 `IR_AnalysisVersion()` 运行期版本查询；Windows DLL 同时内嵌 `VERSIONINFO` 文件版本资源。公共状态码与 UTF-8 错误缓冲区约定见 `IR_status.h`。当前发行结构提供 C、C++、C#、Python 示例、统一 `schemav` 工位/配方模板和 JSON 配方命令行。客户程序只需要两个 SDK 库及所用语言的运行环境。

## 四层代码入口

| 层 | 改动入口 | 输入和输出 |
|---|---|---|
| 仪器控制 | `station/instruments/` 按设备型号接入，`transport/asio_link.cpp` 共用 Asio TCP 与串口 | `IR_OpenDevice` 接收型号和连接 JSON，黑体、IT6332A 电源（支持 `host` TCP / `port` 串口二选一，以及 `IR_WaitPowerCurrentStable` / `IRPowerCurrentStableSpec` 启动冲击确认与稳态电流保持等待）、GWS STEN 烘箱（`IR_OVEN_GWS_STEN`：`IR_SetOven`/`IR_ReadOven`/`IR_StopOven`/`IR_WaitOvenStable`）、Keithley DMM6500 SCPI（`IR_DMM6500`：`IR_SetDmmMode`/`IR_ReadDmm`）、探测器 `0xCC` 串口控制（`IR_DETECTOR_SERIAL`：`IR_SetDetectorTiming`/`IR_SetDetectorGpol`/`IR_SetDetectorRegister`/`IR_MeasureDetectorCurrent` 7 字节 ADC 电流读取）与制冷机 Modbus RTU（`IR_COOLER_MODBUS`：`IR_ReadCooler`，默认 `8N2`）；`IR_RunNoise` 支持 `recipe->detector != 0` 下发积分/空扫时间（`IR_NOISE_STEP_SET_DETECTOR`）；`IR_ListSerialPorts` 枚举可用串口 |
| 原始采集 | `station/acquisition/frame_source.h`，具体来源在 `raw_replay/`、`directshow/`、`v4l2/` | `IR_ListCaptureDevices` 枚举相机；`IR_OpenFrames` 返回几何，`IR_ReadFrames` 输出紧密排列的 RAW16 像素；`IR_FlushFrames` 支撑不停流多 Burst 采集；`IR_TryReadLatestFrame` / `IR_GetCaptureStats` 提供无阻塞预览与有界 `FrameQueue` 诊断 |
| 指标计算 | `analysis/`，可独立使用 `IR_analysis` | `IR_FrameStatisticsU16`、`IR_ROIMeanU16`、`IR_Noise3D*`、`IR_Gb*` / `IR_Gjb*`（含可配置短路/断路阈值）、`IR_FlickerDetectU16` 闪元检测，以及两点 NUC、无损 NUC 统计、`15×15` 空间高低频（盒式均值与掩码归一化高斯 `IR_SPATIAL_FILTER_GAUSSIAN`）、算术扣噪/阈值掩码、任意长 1D FFT、任意尺寸纯 C++20 2D FFT（`IR_Fft2dMagnitudeF64` / `IRFft2dSummary`）与 `IR_GjbEvaluateBurstMetrics` |
| 流程编排 | `station/procedures/`、`station/plugins/` | `IR_RunRecipeJson` / `IR_RunRecipeJsonEx`（支持 `IRRecipeStepCallback` 步骤事件回调）统一采用最新 `schemav` 流程格式（多温点/多条件、`power_wait_stable`、`oven_stabilize`/`oven_standby`/`dmm_read`/`detector_timing`/`detector_gpol`/`detector_register`/`detector_current`/`cooler_read`、中间图与掩码跨步引用、`flush_frames`、可选 `save_raw`、`repeat_burst` 周期无存盘温漂、开放 16 项内置算子及通过 `station.plugins` + `IR_plugin_abi.h` 加载的 `plugin:<ns>.<name>` 外部动态算子、`step_records` 逐步耗时记录及 `safety.blackbody_standby`/`safety.oven_standby` 安全待机收尾）；`IR_WaitBlackbodyStable`、`IR_WaitOvenStable` 与 `IR_WaitPowerCurrentStable` 提供独立等稳接口 |

## 线程和设备占用

一个进程同时打开一个采集源。打开、启动、读取（`IR_ReadFrames`）、冲刷（`IR_FlushFrames`）、停止、关闭和同步流程由打开源的同一工作线程调用，并发操作采集流会返回 `IR_BUSY`；同时实时源由有界 `FrameQueue`（默认 64 帧，可通过 `IR_SetFrameQueueCapacity` 在非运行期调整）在后台泵线程持续拉帧，UI 或监控线程可随时调用 `IR_TryReadLatestFrame`（丢旧保新快照）与 `IR_GetCaptureStats` 无阻塞获取最新帧与实时帧率/队列状态，不会触发 `IR_BUSY`。离线计算可以放在其他线程，每次调用使用自己的输入和输出缓冲区。

`IR_RunNoise` 在第一次设备命令前预约配方指定的仪器。预约期间，其他线程对这些句柄执行读、写或关闭都会得到 `IR_BUSY`。配方释放资源后即可再次操作。未被配方使用的仪器不需要打开。预约只约束当前 SDK 进程内的调用；其他进程的占用由设备或驱动决定。`IR_RequestStop` 可从其他线程调用，设备命令仍由工作线程收尾。实时取帧按最多 50 ms 的等待段检查取消，每帧仍遵守调用方指定的总超时。

`IR_RunRecipeJson` 为 C/C# / Python 提供统一的 UTF-8 工位与流程 JSON 入口，采用最新 `schemav` 格式（`schemav` 默认 `1`，可省略）。`IR_CheckRecipeJson` 做静态预检（校验设备别名、步骤顺序、输入/输出槽位名称、类型与安全参数）。运行入口先按实际画幅检查全部捕获批次容量与所有指标 ROI/通道约束，准备成功后才打开并预约配方引用的仪器。统一 `schemav` 流程支持多温点/多条件与采集交错编排、`save_raw` 可选（0 次即无存盘，或多次按 `subdir` 归档）、`repeat_burst` 周期流式追加 CSV 摘要（中断/取消时保留已刷盘数据），以及开放指标参数透传。一进程同时运行一条配方，每批 RAW 像素默认上限为 256 MiB（可在 `station.acquisition.max_batch_bytes` 调整，队列容量由 `station.acquisition.queue_capacity` 调整，默认 64）。

跨线程停止作用于整次配方，由原工作线程在当前同步事务结束后检查并收尾。启用电源输出时，配方须同时声明 `off_after_run: true`；`schemav` 另可在 `safety.blackbody_standby` 声明结束/异常时统一恢复的黑体待机温度，并在 `safety.oven_standby`（或 `oven_stabilize` 步骤的 `standby_after_run: true`）声明结束后自动切回 `STANDBY` 的烘箱别名。`IRRecipeResult` 报告失败阶段、步骤下标、完成帧数和收尾状态。完整结果归档会保留实际配方、`raw_saved` / `saved_batches` / `burst_runs`、指标方法版本以及 `station_version`、`analysis_version`、`source_commit`、`started_utc`、`finished_utc` 追溯字段。详见[测试人员配方指南](docs/测试人员配方指南.md)。

DOC058 黑体连接中断或应答超时后最多重连三次，每次间隔 500 ms。重复读命令可在重新识别设备后执行。设温应答丢失时，只重连并读回目标温度，不重复发送设温命令。重试耗尽或设备身份不符时，流程停止并返回异常。流程失败时 `IRNoiseResult.completed` 保留连续完成帧数，`failed_step` 标明失败步骤，`cleanup_status` 报告采集停止结果。`IR_RunNoise` 固定流程保留配方黑体设定值，不擅自修改温度；需要自动恢复待机温度或烘箱待机时，使用 `schemav` 配方的 `safety.blackbody_standby` / `safety.oven_standby`。

安装包的 `examples/noise_recipe.c` 演示固定噪声 C API。`ir_run_recipe` 与 C#/Python 的配方例子通过 `IR_RunRecipeJson` 使用同一执行器，覆盖设备等稳、取帧、多个指标和归档。自动测试使用 RAW16 小数组、`simulators/ir_sim_devices.py` 单机模拟及 `simulators/ir_virtual_station.py` 组合式虚拟工位（覆盖全 6 类设备耦合 HIL 与 `CTRL_BREAK_EVENT` 取消收尾）。

安装包的 `examples/analysis_conditions.c` 演示同一个分析入口如何接收不同积分时间下的两组帧，并逐组返回均值和噪声。它只验证数据组织方式，不代表 GJB 或 EMVA 的指标实现。

`IR_ROIMeanU16` 是算法扩展样板：用两帧 `2×2` 数据验证左列 ROI 均值 41 DN、整图均值 51 DN。包内的 `examples/analysis_algorithms.c`（独立链接 `IR_analysis`，不依赖 `IR_station`）、`examples/python/ir_analysis.py` 与 `examples/csharp/AnalysisInterop.cs` 演示了独立 `IR_analysis` 库中的 3D 噪声（Noise3D DN 两阶段）、国标（GB，支持在 `IRGbCriteria` 配置短路/断路阈值，`<= 0` 时默认 `45000 / 20000`）/ 国军标（GJB）、闪元检测（`IR_FlickerDetectU16`）以及可组合算子（两点 NUC、`15×15` 盒式/高斯空间高低频、掩码组合、任意长度 1D FFT、任意尺寸纯 C++20 2D FFT `IR_Fft2dMagnitudeF64`、多通道频谱与单 Burst 轻量评估）C 接口调用；此外包内提供通用硬件验收与长稳拷机命令行 `ir_capture_accept`（支持 `--list` 枚举与多轮启停/`--flush-between` 连续流 + 并发无阻塞预览压测）。其中 GB/GJB 噪声图 `vn` 使用总体标准差（`IR_FrameStatisticsU16(..., sample_std = 0)`，分母 `N`），而既有 `IR_RunNoise` 与 `temporal_noise` 保持样本标准差（`sample_std = 1`，分母 `N - 1`）不变。对于 5 小时温漂等不停流、不存盘的多 Burst 场景，应用可在单工位循环内复用 `IR_WaitBlackbodyStable`、`IR_WaitPowerCurrentStable`、`IR_FlushFrames`、`IR_ReadFrames` 与 `IR_FrameStatisticsNucU16` / `IR_GjbEvaluateBurstMetrics`。源码仓库内提供面向算法研发的完整接入指南与测试模板。

## SDK 依赖和构建

SDK 运行时不依赖 Qt。Asio 和 nlohmann/json 只参与 `IR_station` 与配方命令行的编译，分别以头文件形式编入目标程序，不需要安装 Asio 或 JSON 动态库。nlohmann/json 3.12.0 单头文件及其 MIT 许可证随源码和交接包提供。standalone Asio 1.36.0 及 Boost Software License 1.0 位于 `third_party/asio/`，干净检出即可构建；客户 SDK 安装包不复制 Asio 源码。

Windows 客户程序部署 `IR_station.dll`、`IR_analysis.dll` 和相应的 MSVC 运行库。Linux 部署 `libIR_station.so`、`libIR_analysis.so` 和系统 C/C++ 运行库。`IR_station` 会在同目录加载 `IR_analysis`。这两个平台包都不携带 Qt。

构建 SDK 需要 CMake 3.21 或更新版本、Ninja 和 C++20 编译器。Python 3 用于 TCP 模拟和 Linux 虚拟串口自动检查，NumPy 仅用于真实 RAW 指标对照。C# 示例面向 .NET 10；Python 示例只用标准库。CMake 默认使用仓库内 Asio；需要验证另一份 Asio 时可传入 `-DIR_ASIO_INCLUDE_DIR=<目录>`。

```text
cmake --preset linux -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build --preset linux
ctest --preset linux
cmake --install 实验/build-linux --prefix 实验/sdk-linux
```

Windows 在 x64 MSVC 开发终端使用 `windows` Debug preset（`CMakePresets.json` 仅保留 Windows Debug 构建配置）。公共头文件和字符串参数使用 UTF-8；MSVC 编译 C/C++ 示例时启用 `/utf-8`。示例的独立编译和调用方法见 [`examples/README.zh-CN.md`](examples/README.zh-CN.md)。

PCIe-1433 暂不编入 SDK。公开枚举保留 `IR_NI_IMAQ = 4`，当前调用 `IR_OpenFrames` 会返回 `IR_UNSUPPORTED`。后续确认目标 Windows 设备、NI-IMAQ 版本和接口配置后，可在该采集入口实现后端；这项 Windows API 兼容设计不代表 Linux/WSL 可使用相同驱动。Linux 支持要按具体板卡、驱动和 SDK 组合另行确认。

## 实时图像采集

`IR_OpenFrames` 按平台选择实时相机或 RAW 回放：Windows 传 `IR_DIRECTSHOW` 和相机名（例如 `FX3`）；Linux 传 `IR_V4L2` 和视频节点（例如 `/dev/video0`）；离线数据使用 `IR_RAW_REPLAY` 和目录路径。`IR_NI_IMAQ` 是为 PCIe-1433 预留的编号，当前返回 `IR_UNSUPPORTED`。读取顺序为打开、启动、读取、停止、关闭。

Windows 后端沿用 USB_test 使用的 DirectShow Sample Grabber COM 过滤器，部署电脑需要注册 `qedit.dll`。Microsoft 已将该过滤器标记为旧版并弃用；当前保留它是为了复用现有 FX3 采集路径。[Microsoft Sample Grabber 文档](https://learn.microsoft.com/en-us/windows/win32/directshow/sample-grabber-filter)

两个实时后端只接受设备提供的 16 位原始传输格式，不做 RGB/YUV 彩色转换。Windows 接受 FX3 的 RGB565 或 YUY2 媒体类型；Linux 接受 FX3 的 YUYV 格式。数据按行紧密排列为 `uint16_t` 像素。实时源的有效位数由调用方按探测器配置传入；传 0 表示未知。V4L2 会跳过空帧和不完整帧；读取过程中发现帧序号跳变时返回 `IR_FRAME_GAP`，`completed` 和对应缓冲区前缀仍有效。Windows DirectShow 也会记录回调队列中的帧序号跳变。

相机帧采集和探测器参数/开流命令可协同或分步执行。`1.6.0` 已在 `IR_DETECTOR_SERIAL` 接入 `0xCC` 串口时序（`IR_SetDetectorTiming`）、GPOL（`IR_SetDetectorGpol`）、通用寄存器下发（`IR_SetDetectorRegister` / Schema v2 `detector_register`）及 `0x41` 7 字节 ADC 电流读取（`IR_MeasureDetectorCurrent`），可配合 `IR_StartFrames` / `IR_ReadFrames` 联调；真机具体寄存器定义与开流时序仍需按实际探测器型号核对。

## 仪器和流程

调用 `IR_OpenDevice` 时传设备类型和一份连接 JSON。例如：

```json
{"host":"192.168.1.20","tcp_port":2101,"head":0,"expected_head_serial":"HEAD01","timeout_ms":3000}
```

```json
{"port":"COM5","timeout_ms":3000}
```

第一份用于 PCN7-LT（`IR_PCN7LT`）。`host` 必填，TCP 端口默认 2101，`head` 默认 -1，表示命令不带探头索引。配置了序列号校验时须指定探头 0 或 1。第二份用于 DCN1000（`IR_DCN1000`）。Linux 把 `port` 填为实际设备路径。有 `/dev/serial/by-id/` 链接时可直接使用。串口固定为 9600、8N1、无流控。`timeout_ms` 以毫秒计，默认 3000，范围 1 到 60000，作用于单条指令的写入和应答。

1. 打开黑体并取得设备句柄。打开时核对控制器型号、序列号和适用的探头信息。用 `IR_SetBlackbody` 设温并读回，用 `IR_ReadBlackbody` 获取实际温度、设定温度和稳定标志。温度单位为 °C。
2. 用 `IR_OpenFrames` 打开带索引的 RAW 目录，得到宽、高和有效位。直接读帧可依次调用 `IR_StartFrames`、`IR_ReadFrames`、`IR_StopFrames`。
3. 用 `IR_RunNoise` 运行固定流程。离线配方将两个设备句柄都填 0。带黑体时填已打开的句柄，依次设温、读回、等稳、采集指定帧数、计算像元样本标准差。稳定窗口要求连续读数满足温差和可选的设备稳定标志。失败时保留完成帧数，指标无效。
4. 指定一个新目录即可保存 `frames.raw`、`frames.csv` 和 `result.json`。结果文件记录配方设定、设温回读和达到等稳时的实际温度/设备稳定标志。三个文件齐备后发布目录。再次打开保存目录可复算同一批数据。
5. 用 `IR_CloseFrames` 和 `IR_CloseDevice` 释放连接。DOC058 黑体的串口和 TCP 读操作在传输失败后都会重新识别设备，最多重试三次。设温应答超时通过回读确认，SDK 不重发该写命令。`IR_RequestStop` 在当前仪器同步调用结束时中止流程。

当前发行版本（`1.8.0`）提供 RAW 回放、DOC058 TCP/串口黑体、IT6332A 串口/TCP 电源（含 `IR_WaitPowerCurrentStable` 电流稳态等待）、GWS STEN 烘箱（TCP/串口）、Keithley DMM6500 SCPI 万用表（TCP/串口）、探测器 `0xCC` 串口控制器（含 `IR_SetDetectorRegister` 与 `IR_MeasureDetectorCurrent`）、制冷机 Modbus RTU（`IR_COOLER_MODBUS`，默认 `8N2` TCP/串口）、时域噪声/ROI 均值/Noise3D/GB/GJB/闪元检测/高斯滤波与纯 C++20 2D FFT（`IR_Fft2dMagnitudeF64`）C API、Schema v1/v2 JSON 配方流程（含 `IR_RunRecipeJsonEx` 步骤事件回调、`step_records` 逐步记录与 `IR_plugin_abi.h` 外部算子插件加载）、组合式虚拟工位 `simulators/ir_virtual_station.py`（支持 TCP 与 6 路虚拟 COM 在环），以及 DirectShow/V4L2 实时后端接口。PCIe-1433 的 NI-IMAQ 运行实现暂留空，接口编号已保留；FX3 DirectShow/V4L2 实机长稳、物理硬件与真实串口设备、EMVA 1288 指标仍需按实际设备与对照数据逐项验收。各能力与平台的实际支持状态见[设备支持矩阵](docs/设备支持矩阵.md)。
