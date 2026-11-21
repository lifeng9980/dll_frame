# IR SDK 调用示例

客户程序只包含 SDK 公共头文件或自行声明 C ABI，并链接 `IR_station` 与 `IR_analysis`。Windows 部署目录放置两个 DLL，Linux 放置两个 `.so`；Asio、JSON 解析和文件实现都在 SDK 内部，调用程序不需要安装 Qt、Asio 或 nlohmann/json。

## 1. 命令行配方

`bin/ir_run_recipe` 读取 JSON 配方并调用公开 C API。`recipes/noise_offline.json` 使用随包提供的两帧 RAW，计算完后把 `frames.raw`、`frames.csv` 和 `result.json` 保存到 `output/noise-offline-example`。

PowerShell：

```powershell
& '.\windows-x64\bin\ir_run_recipe.exe' '.\recipes\noise_offline.json'
```

Ubuntu：

```bash
./linux-x86_64/bin/ir_run_recipe ./recipes/noise_offline.json
```

相对 RAW 与输出目录都以配方 JSON 所在目录为基准。目录路径可包含中文和空格。结果目录必须是新目录，父目录必须已存在。

要在 Windows 试相机和黑体，复制 `recipes/noise_directshow.template.json`，填写系统设备列表中的 DirectShow 名称、串口和产品配方，再运行 CLI。Linux 使用 `noise_v4l2.template.json`，采集源例如 `/dev/video0`。PCN7-LT TCP 示例见 `noise_mock_tcp.template.json`。模板中的串口、相机、IP、序列号和输出目录都需要按现场修改。

## 2. C 与 C++ 客户端

`examples/CMakeLists.txt` 只依赖 CMake 和 C/C++ 编译器，可分别编译纯 C `ir_c_client`、C++ `ir_cpp_client` 和配方命令行。它从 `IR_SDK_ROOT` 指定的解压目录读取头文件、导入库和运行库。

```powershell
cmake -S .\examples -B .\build-client -DIR_SDK_ROOT="$PWD"
cmake --build .\build-client --config Release
```

```bash
cmake -S examples -B build-client -DIR_SDK_ROOT="$PWD"
cmake --build build-client --config Release
```

## 3. C# P/Invoke

`csharp/IRSdkSample.csproj` 面向 .NET 10。示例通过 `DllImport` 调用 C ABI，使用 `ulong` 表示 64 位设备句柄、`IntPtr` 表示 C 字符串指针，并以 UTF-8 传递路径和配方名。两个 native 库不需要包装成 C++/CLI。

Windows：

```powershell
$env:IR_SDK_LIB_DIR = (Resolve-Path '.\windows-x64\bin').Path
dotnet run --project .\examples\csharp\IRSdkSample.csproj -- .\data\raw16 .\output\csharp-run
```

Ubuntu：

```bash
export IR_SDK_LIB_DIR="$PWD/linux-x86_64/lib"
dotnet run --project examples/csharp/IRSdkSample.csproj -- data/raw16 output/csharp-run
```

传入输出目录时它必须尚不存在。省略输出目录只返回计算结果。

## 4. Python ctypes

`python/ir_noise.py` 只用 Python 标准库。默认从本包平台目录加载 `IR_station`，也可用 `IR_SDK_LIB_DIR` 或 `--library-dir` 指定 SDK 库目录。

```powershell
& 'D:\CS\py\.venv_3_12\Scripts\python.exe' .\examples\python\ir_noise.py .\data\raw16
```

```bash
python3 examples/python/ir_noise.py data/raw16 --output output/python-run
```

## 5. 本地编译要求

- SDK 客户端：C/C++ 编译器和 CMake。公开 C 头文件不依赖 Qt、Asio 或 JSON 库。
- C# 示例：.NET 10 SDK。
- Python 示例：Python 3.9 或更新版本；只用标准库。
- SDK 本身：CMake、C++20 编译器和 standalone Asio 头文件。JSON 单头文件已随源码包提供。Asio 只在构建时使用，编入 `IR_station`，不产生 Asio 动态库依赖。

SDK 公共函数为同步调用。采集源的打开、流程运行与关闭应由同一个工作线程执行；只允许从其他线程调用 `IR_RequestStop`。调用程序不应在另一线程同时控制流程当前占用的仪器。
