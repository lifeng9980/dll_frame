# IR SDK 调用示例

SDK 提供两个库：`IR_station` 负责设备、RAW16 采集和流程，`IR_analysis` 负责独立指标计算。公共接口是纯 C ABI，C、C++、C# P/Invoke 和 Python `ctypes` 都可以直接调用；客户程序不需要 Qt、Asio 或 nlohmann/json 动态库。

## JSON 测试流程

工位配置列出采集源和可选设备；配方按顺序引用设备、采集一个批次、运行一个或多个指标、设置限值并归档。先预检再运行：

```powershell
& '.\windows-x64\bin\ir_run_recipe.exe' --check --station '.\examples\stations\offline.json' --recipe '.\examples\recipes\noise_roi.json' --output '..\output\offline-run'
& '.\windows-x64\bin\ir_run_recipe.exe' --run --station '.\examples\stations\offline.json' --recipe '.\examples\recipes\noise_roi.json' --output '..\output\offline-run'
```

```bash
./linux-x86_64/bin/ir_run_recipe --check --station examples/stations/offline.json --recipe examples/recipes/noise_roi.json --output ../output/offline-run
./linux-x86_64/bin/ir_run_recipe --run --station examples/stations/offline.json --recipe examples/recipes/noise_roi.json --output ../output/offline-run
```

随包例子包括：

- `stations/offline.json` + `recipes/noise_roi.json`：离线 RAW、时域噪声、ROI 均值和产品限值。
- `stations/roi_2x2.json` + `recipes/noise_roi_2x2.json`：两帧 `2×2` 数据，整图均值 `51 DN`，左列 ROI 均值 `41 DN`。
- `stations/sim_tcp.json` + `recipes/noise_blackbody.json`：TCP 黑体设温等稳后采集。
- `stations/sim_power_serial.json` + `recipes/power_serial_smoke.json`：IT6332A 串口设定/回读后采集，不打开输出。
- `stations/sim_power_serial.json` + `recipes/power_serial_output_cycle.json`：在模拟工位练习打开输出，取帧后关闭并回读确认。

CLI 调用公开的 `IR_CheckRecipeJson` 与 `IR_RunRecipeJson`。C# 和 Python 的 `RecipeRunner` 示例也直接调用这两个入口，不需要自行实现设备和采集逻辑。输出目录需是新目录；结果的 `PASS`、`NG` 和 `COMPLETED` 含义见[测试人员指南](../docs/测试人员配方指南.md)。

## C 与 C++ 客户端

`examples/CMakeLists.txt` 从 `IR_SDK_ROOT` 指定的解压目录加载公共头文件、导入库和运行库，构建纯 C（`ir_c_client`、`ir_analysis_c_sample`）、C++（`ir_cpp_client`）和流程 CLI（`ir_run_recipe`）样例；其中 `ir_analysis_c_sample`（源码对应 `analysis_algorithms.c`）**仅链接并运行 `IR_analysis`**（不依赖 `IR_station`），演示 `IR_Noise3D*`、`IR_Gb*` / `IR_Gjb*`、两点 NUC、`15×15` LRNU/HRNU、1D FFT 与单 Burst 温漂评估及 ABI 结构体大小校对：

```powershell
cmake -S .\examples -B .\build-client -DIR_SDK_ROOT="$PWD"
cmake --build .\build-client --config Release
```

```bash
cmake -S examples -B build-client -DIR_SDK_ROOT="$PWD"
cmake --build build-client --config Release
```

## C# P/Invoke

`csharp/IRSdkSample.csproj` 面向 .NET 10。它包含传统 RAW/噪声调用、JSON 流程调用（`RecipeRunner.cs`）以及 `IR_analysis` 核心算法 P/Invoke 声明（`AnalysisInterop.cs`）：

```powershell
$env:IR_SDK_LIB_DIR = (Resolve-Path '.\windows-x64\bin').Path
dotnet run --project .\examples\csharp\IRSdkSample.csproj -- --recipe .\examples\stations\offline.json .\examples\recipes\noise_roi.json ..\output\csharp-run
dotnet run --project .\examples\csharp\IRSdkSample.csproj -- --analysis
```

```bash
export IR_SDK_LIB_DIR="$PWD/linux-x86_64/lib"
dotnet run --project examples/csharp/IRSdkSample.csproj -- --recipe examples/stations/offline.json examples/recipes/noise_roi.json ../output/csharp-run
dotnet run --project examples/csharp/IRSdkSample.csproj -- --analysis
```

## Python ctypes

示例只使用 Python 标准库（`ir_noise.py`、`ir_recipe.py` 与调用 Noise3D/GB/GJB 的 `ir_analysis.py`）：

```powershell
python .\examples\python\ir_recipe.py --check .\examples\stations\offline.json .\examples\recipes\noise_roi.json ..\output\python-run
python .\examples\python\ir_recipe.py --run .\examples\stations\offline.json .\examples\recipes\noise_roi.json ..\output\python-run
python .\examples\python\ir_analysis.py
```

```bash
python3 examples/python/ir_recipe.py --check examples/stations/offline.json examples/recipes/noise_roi.json ../output/python-run
python3 examples/python/ir_recipe.py --run examples/stations/offline.json examples/recipes/noise_roi.json ../output/python-run
python3 examples/python/ir_analysis.py
```

通过 `IR_SDK_LIB_DIR` 可指定库目录。客户代码直接传 UTF-8 JSON 字符串给 C ABI，不需要自行链接 JSON 解析库。

## Schema 校验与算法扩展

- `examples/schemas/station.schema.json` 与 `examples/schemas/recipe.schema.json` 提供 JSON Schema（Draft 2020-12）定义，随包工位与配方模板已通过 `"$schema"` 引用，可在编辑器中直接校验字段。
- 源码级二次开发时，新增指标放在 `analysis/`，公共 C 函数放在 `include/IR_analysis.h`，小数据预期结果放在 `sdk_tests/`；供测试配方调用的方法统一登记在 `station/procedures/recipe_metrics.cpp`（详细开发与算法接入规范位于 SDK Git 源码仓库的 `docs/` 目录）。
