# IR SDK 跨平台重建

`codex/sdk-cross-platform` 从独立空提交开始。Windows x64 与 Ubuntu 24.04 x86-64 使用同一份源码和公共 C 头文件，生成两个库。

| 库 | Windows | Linux | 职责 |
|---|---|---|---|
| Station | `IR_station.dll` | `libIR_station.so` | 仪器控制、原始帧回放、固定噪声流程与结果保存 |
| Analysis | `IR_analysis.dll` | `libIR_analysis.so` | 帧均值和时域噪声 |

应用包含 `IR_station.h`、`IR_device.h` 或 `IR_analysis.h`，以 `IR_*` 调用 C 接口。公共状态码与 UTF-8 错误缓冲区约定见 `IR_status.h`。安装包含纯 C 示例 `examples/c_client.c`。

## 四层代码入口

| 层 | 改动入口 | 输入和输出 |
|---|---|---|
| 仪器控制 | `station/instruments/blackbody/` 解释 DOC058，`transport/asio_link.cpp` 共用 Asio TCP 与串口 | `IR_OpenDevice` 接收型号和连接 JSON，`IR_SetBlackbody` 设温并回读，`IR_ReadBlackbody` 读取实际温度和状态 |
| 原始采集 | `station/acquisition/frame_source.h`，具体来源在 `raw_replay/`、`directshow/`、`v4l2/` | `IR_OpenFrames` 返回几何，`IR_ReadFrames` 输出 RAW16 像素、帧信息和连续完成数 |
| 指标计算 | `analysis/frame_statistics.cpp`，可独立使用 `IR_analysis` | `IR_FrameStatisticsU16` 接收连续帧组，返回像元均值、标准差及全图汇总，单位 DN |
| 流程编排 | `station/procedures/noise_run.cpp` | `IR_RunNoise` 依配方设温、等稳、取帧、算指标，并按需保存。量产调用方按产品限值判定 |

安装包的 `examples/noise_recipe.c` 给出了四层联用的纯 C 示例。它从当前目录读取 RAW，使用第一个参数 `tcp` 或 `serial` 选择黑体，第二个参数传连接 JSON。示例会向所选设备发送 32 °C 设温命令。自动测试用本机模拟黑体配合 RAW 回放，输入为 20 和 22 DN，预期均值 21 DN，样本时域噪声约 1.4142135623730951 DN。设备未稳定时示例返回超时和零帧。

## 构建和依赖

使用 CMake 3.21 或更新版本、Ninja、C++20 编译器、QtCore 5.15 开发包和 standalone Asio 头文件。Asio 只在编译 Station 时使用。找不到 `asio.hpp` 时传入 `-DIR_ASIO_INCLUDE_DIR=<目录>`。QtCore 用于 Station 的连接 JSON 和结果文件。RAW 解码和 Analysis 无 Qt 依赖。

```text
cmake --preset linux -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build --preset linux
ctest --preset linux
cmake --install 实验/build-linux --prefix 实验/sdk-linux
```

Windows 在 x64 MSVC 终端使用 `windows` preset，将 Qt 路径传给 `CMAKE_PREFIX_PATH`。Python 3 用于模拟通信测试，实际 RAW 指标对照还需要 NumPy。Windows 安装包附带 Qt5Core，应用需要 MSVC 运行库。公共头文件使用 UTF-8，用 MSVC 编译 C 示例时传入 `/utf-8`。Linux 使用系统安装的 QtCore 5.15。

## 实时图像采集

`IR_OpenFrames` 按平台选择实时相机或 RAW 回放：Windows 传 `IR_DIRECTSHOW` 和相机名（例如 `FX3`）；Linux 传 `IR_V4L2` 和视频节点（例如 `/dev/video0`）；离线数据使用 `IR_RAW_REPLAY` 和目录路径。读取顺序为打开、启动、读取、停止、关闭。

Windows 后端沿用 USB_test 使用的 DirectShow Sample Grabber COM 过滤器，部署电脑需要注册 `qedit.dll`。Microsoft 已将该过滤器标记为旧版并弃用；当前保留它是为了复用现有 FX3 采集路径。[Microsoft Sample Grabber 文档](https://learn.microsoft.com/en-us/windows/win32/directshow/sample-grabber-filter)

两个实时后端只接受设备提供的 16 位原始传输格式，不做 RGB/YUV 彩色转换。Windows 接受 FX3 的 RGB565 或 YUY2 媒体类型；Linux 接受 FX3 的 YUYV 格式。数据按行紧密排列为 `uint16_t` 像素。实时源的有效位数由调用方按探测器配置传入；传 0 表示未知。V4L2 会跳过空帧和不完整帧；读取过程中发现帧序号跳变时返回 `IR_FRAME_GAP`，`completed` 和对应缓冲区前缀仍有效。Windows DirectShow 也会记录回调队列中的帧序号跳变。

相机帧采集和探测器开流命令是两个独立动作。调用方先启动实时采集，再通过探测器接口打开数据流，最后调用 `IR_ReadFrames`。当前 SDK 还没有纳入面阵测试串口开关命令；本次硬件联调使用 SSCOM 配置中的测试命令只验证了链路，不代表最终上位机协议。

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
4. 指定一个新目录即可保存 `frames.raw`、`frames.csv` 和 `result.json`。三个文件齐备后发布目录。再次打开保存目录可复算同一批数据。
5. 用 `IR_CloseFrames` 和 `IR_CloseDevice` 释放连接。TCP 读操作可在传输失败后重新识别设备并重试一次。设温应答超时通过回读确认，SDK 不重发该写命令。`IR_RequestStop` 在当前仪器同步调用结束时中止流程。

当前分支提供双平台 RAW 回放、黑体通信和固定时域噪声流程；Windows DirectShow 与 Linux V4L2 均已实现并完成编译。Linux UVC 真机已读到 FX3 测试坡度图，但在 WSL USB/IP 下多帧接口检测到序号跳变，因此不计为连续采集通过。探测器参数控制、其他仪器、GB/GJB 和专项算法仍需按实际设备与对照数据验收。见 [验证记录](验证记录.md)。
