# IR SDK 跨平台重建

本分支从独立空提交开始。Windows x64 与 Ubuntu 24.04 x86-64 使用同一份源码和公共 C 头文件，构建 `IR_station` 与 `IR_analysis` 两个库。

当前首个闭环支持 RAW 目录打开、读帧、时域噪声计算、结果保存和回放。采集索引解码和逐像元时域统计复用既有 SDK 中已验证的逻辑。串口、TCP、实时取帧和专项算法按工位验收顺序接入。

## 构建产物

| 平台 | 库文件 |
|---|---|
| Windows | `IR_station.dll`、`IR_analysis.dll` 和对应的 `.lib` 导入库 |
| Linux | `libIR_station.so`、`libIR_analysis.so` |

应用通过 `IR_station.h` 或 `IR_analysis.h` 引用 C 接口。公共状态码定义在 `IR_status.h`，导出函数使用 `OptMeas_*`。

## 构建

使用 CMake 3.21 或更新版本、Ninja、C++20 编译器和 QtCore 5.15 开发包。Windows 从 x64 MSVC 终端配置 `windows` preset，Qt 安装路径由本机传给 `CMAKE_PREFIX_PATH`。WSL 使用 `linux` preset。

```text
cmake --preset linux
cmake --build --preset linux
ctest --preset linux
cmake --install 实验/build-linux --prefix 实验/sdk-linux
```

Windows 将以上命令中的 `linux` 改成 `windows`。保存目录由调用方指定并须为新目录。

## 读帧和测量

1. 用 `OptMeas_OpenFrames` 打开带索引的 RAW 目录，得到实际宽、高和有效位。
2. 用 `OptMeas_RunNoise` 执行离线测量，或用 `StartFrames`、`ReadFrames`、`StopFrames` 依次读取，然后调用 Analysis。离线配方的两个设备句柄填 0。
3. 需要保存时给一个新的输出目录。SDK 写入 `frames.raw`、`frames.csv`、`result.json`。
4. 用 `OptMeas_CloseFrames` 结束采集。公共头文件描述线程、输出容量、单位和异常状态。
