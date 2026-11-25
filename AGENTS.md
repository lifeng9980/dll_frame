# AGENTS.md

`IR SDK`（C++20，Windows x64 / Linux x86_64 源码可移植）是面向红外探测器自动化测试与图像分析的**纯 C ABI 双动态库框架**（`IR_analysis` + `IR_station`）。文档采用 4 份单一真相源：`AGENTS.md`（架构与开发规范）、[SDK.md](SDK.md)（客户总览）、[客户接入说明.md](docs/客户接入说明.md) 与 [研发与算法指南.md](docs/研发与算法指南.md)。

---

## 1. 定位与收束红线（Non-Goals）

- **不做 UI/HMI 与工单**：界面交互、良率阈值与工单编排留在上位机；SDK 只暴露纯 C ABI 与 CLI，并在失败/取消时输出诊断 `result.json`。
- **不拆分微库**：除 `IR_analysis`、`IR_station` 及按 `IR_plugin_abi.h` 扩展的外部算子插件外，不为单个仪器或内置算子额外拆 DLL/SO。
- **零重型运行依赖**：核心双库保持**零 Qt、零 OpenCV、零第三方运行时动态库**；`Asio 1.36.0` 与 `nlohmann/json 3.12.0` 仅作私有编译期头依赖，严禁进入 `include/`。
- **不破坏兼容语义**：已发布指标口径（如 `temporal_noise` 逐像元样本标准差 `N - 1` vs GJB 总体标准差 `N`）不得静默篡改；新能力走独立指标名或外部插件入口。

---

## 2. 目录分类与紧凑内聚约束

| 目录 | 定位与允许内容 | 禁止放入 |
|---|---|---|
| `include/` | 纯 C99/C11 稳定 ABI 头（`IR_*.h`）与可选 C++ 插件守卫 `IR_plugin_cxx.hpp` | STL/Asio/nlohmann 类型、内部实现细节 |
| `shared/` | 内部共用的最小非公开辅助（`api_common.h`、`utf8_path.h`） | 业务逻辑、硬件/采集/流程状态 |
| `analysis/` | `IR_analysis` 无状态纯数学与图像算子（`basic_stats.cpp`、`noise3d_dn.cpp`、`gb_gjb.cpp`、`composable_ops.cpp`、`spectral_ops.cpp`、`matrix_view.h`） | 硬件通信、文件/串口 IO、流程或限值判定 |
| `station/instruments/` | 扁平存放 `asio_link`、`drivers.h`、`device_lease.h`、`device_api`/`discovery` 及各仪器协议驱动 | 采集逻辑、流程编排、跨驱动互调、单对子目录 |
| `station/acquisition/` | 扁平存放 `frame_source`、`frame_queue` 与平台后端（`raw_replay_source`/`directshow_source`/`v4l2_source`） | RGB/YUV 转换、伪造帧戳、仪器/配方逻辑、单文件子目录 |
| `station/procedures/` | 流程生命周期、`recipe_{parse,instruments,runner,metrics,json}.cpp`、`metrics_{basic,standard}.cpp`、`measurement`/`save_raw` | 直接包含 `station/plugins/` 实现细节（仅组合根 `recipe_json.cpp` 可装配插件） |
| `station/plugins/` | `plugin_runtime.{h,cpp}`（`PluginMetricProvider`、进程级句柄缓存与就地零拷贝回调） | 配方流程控制流、仪器/采集后端直接依赖 |
| `examples/` · `simulators/` | 多语言客户示例、`schemav` 工位/配方模板、真实数据目录（`examples/data/{tc002a_640,s07_1280}` 不入包不跟踪）与虚拟模拟器 | 内部个人路径、内部分支/关联项目名、人工小仿真 RAW 目录 |
| `sdk_tests/` · `tools/` | 单元测试、模拟/HIL 测试与打包校验脚本（`package_sdk.py`、`verify_windows.ps1`） | — |
| `docs/` · `third_party/` | 客户接入说明与内部研发指南；`third_party/` 仅放私有头文件库 | 内部文档禁止打入客户交付包 |

### 紧凑内聚与防碎片原则
1. **禁止单文件/单对微子目录**：不为单个驱动或单文件后端创建子目录（`station/instruments/` 与 `station/acquisition/` 保持扁平）。
2. **合并微文件**：小于 ~100 行且无独立 ABI/增量编译隔离边界的头源文件应并入所属域模块，避免过度碎片化；单文件建议控制在 ~850 行以内。
3. **测试唯一真相源（SSOT）**：功能回归统一归口 `CMakeLists.txt` + `ctest`，按域合并微测试，严禁按单函数新增独立可执行目标；`examples/` 与 `sdk_tests/` 职责分离；`verify_windows.ps1` 仅负责构建/CTest 调度、二进制/版本检查、打包卫生校验与最小包内冒烟。

---

## 3. 抽象层与单向依赖红线

```text
  L5 绑定与工具    examples/ · simulators/ · tools/          (仅调用 L0 纯 C ABI)
        ▼
  L4 插件适配实现  station/plugins/                          (PluginMetricProvider；由组合根 recipe_json.cpp 注入)
        ▼
  L3 流程抽象编排  station/procedures/                       (依赖 L0、L1、L2 与 ExternalMetricResolver)
        ▼
  L2 硬件与采集    station/instruments/ | station/acquisition/ (互不依赖；均仅依赖 L0)
        ▼
  L1 纯数学计算    analysis/ (IR_analysis)                   (仅依赖 L0，零硬件/IO)
        ▼
  L0 公共纯 C ABI  include/ (IR_status.h ← IR_types.h ← IR_plugin_abi.h / IR_station.h ← IR_device.h / IR_recipe.h)
```

1. `analysis/` **严禁**引用 `station/` 任何头文件；`include/` **严禁**包含 `shared/`、`station/` 或 `third_party/`。
2. `station/instruments/` 与 `station/acquisition/` **严禁**互相引用或反向依赖 `procedures/`、`plugins/`；同层驱动/后端之间**严禁**互相引用。
3. `station/procedures/` **严禁**直接包含 `station/plugins/`（仅 `recipe_json.cpp` 可装配注入 `PluginMetricProvider`）；`station/plugins/` **严禁**直接包含 `station/acquisition/` 或 `station/instruments/`。
4. **跨 DLL 安全**：插件 `extern "C"` 回调使用 `ir_plugin_guard`；宿主回调须 `extern "C" noexcept` 全捕获；读取插件 `errBuf` 前末尾写 `'\0'`；UTF-8 路径统一复用 `shared/utf8_path.h`。
5. **打包卫生**：以 `tools/package_sdk.py` 的 `REQUIRED_CUSTOMER_FILES` / `FORBIDDEN_PACKAGE_FILES` / `FORBIDDEN_PATTERNS` 为准。

---

## 4. 构建、调试与源码追踪

```powershell
# 1. CMake windows Debug preset 配置、构建与 CTest
cmake --preset windows ; cmake --build --preset windows ; ctest --preset windows --output-on-failure

# 2. 全量 Windows Debug 验收（可选 -RunCom0ComHil 启用虚拟串口 HIL，发布前加 -RequireClean）
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify_windows.ps1 -PythonExe "<venv-python>" -RunCom0ComHil
```

- **L5 → L0 源码追踪**：`examples/ir_recipe_cli.cpp` → `include/IR_recipe.h` → `station/procedures/recipe_json.cpp` → `recipe_{parse,instruments,runner}.cpp` → `recipe_metrics.cpp` + `metrics_{basic,standard}.cpp` → `analysis/*.cpp`。
- **调试与产物目录**：产物分置 `实验/build-debug/{bin,lib,lib/plugins,tests/bin}/`；VS Code `F5` 运行 `ir_run_recipe.exe --run --station 实验/debug_tc002a/station.json --recipe 实验/debug_tc002a/recipe.json --output out_dbg`。
