# AGENTS.md

`IR SDK`（`1.8.0`，C++20，Windows x64 / Linux x86_64）是面向红外探测器自动化测试与图像分析的**纯 C ABI 双动态库框架**（`IR_analysis` + `IR_station`）。详细规范见 [开发规范.md](docs/开发规范.md)、[算法接入指南.md](docs/算法接入指南.md)、[SDK.md](SDK.md) 与 [验证记录.md](验证记录.md)。

---

## 1. 定位与收束红线（Non-Goals）

- **不做 UI/HMI 与工单**：界面交互、产品良率阈值与工单编排留在上位机，SDK 只暴露纯 C ABI 与 CLI。
- **不拆分微库**：除 `IR_analysis`、`IR_station` 及客户按 `IR_plugin_abi.h` 自建的算子插件外，不为单个仪器或内置算法额外拆 DLL/SO。
- **零重型运行依赖**：核心双库保持**零 Qt、零 OpenCV、零第三方运行时动态库**；`Asio 1.36.0` 与 `nlohmann/json 3.12.0` 仅作私有编译期头依赖，严禁进入 `include/`。
- **不破坏兼容语义**：`IR_RunNoise` 与 `schemav` `temporal_noise` 固定逐像元样本标准差（分母 `N - 1`）；GJB 总体标准差（分母 `N`）与新能力必须走独立指标名、插件或 `*Ex` 入口。

---

## 2. 目录分类约束（Folder Taxonomy）

| 目录 | 定位与允许内容 | 禁止放入 |
|---|---|---|
| `include/` | 纯 C99/C11 稳定 ABI 头（`IR_*.h`）与可选 C++ 插件守卫 `IR_plugin_cxx.hpp` | STL/Asio/nlohmann 类型、内部实现细节 |
| `shared/` | 内部共用的最小非公开辅助（`api_common.h` 错误码与异常守卫） | 业务逻辑、硬件/采集/流程状态 |
| `analysis/` | `IR_analysis` 无状态纯数学与图像算子（统计、ROI、3D 噪声、GB/GJB、FFT、闪烁） | 硬件通信、文件/串口 IO、流程或限值判定 |
| `station/instruments/` | `transport/asio_link` 统一传输、`device_lease.h` 独占租约、`device_api`/`discovery` 及 6 类仪器驱动 | 采集逻辑、流程编排、跨驱动互调 |
| `station/acquisition/` | `FrameSource` 抽象、`FrameQueue` 有界队列与平台后端（`raw_replay`/`directshow`/`v4l2`） | RGB/YUV 转换、伪造帧戳、仪器/配方逻辑 |
| `station/procedures/` | 流程生命周期、统一 `schemav` 配方解析/预检/执行、`IMetricProviderV2` 抽象、`metrics_v2_*` 与归档 | 具体插件装载细节、仪器协议或采集后端细节 |
| `station/plugins/` | `PluginManager` 动态加载/SHA-256 溯源、`HostCallbackBridge` 沙箱与 `PluginMetricProvider` 实现 | 配方流程控制流、仪器控制 |
| `examples/` · `simulators/` | 多语言示例（C/C++/CLI/Python/C#）、`schemav` 工位/配方模板与虚拟仪器/工位模拟器（含入包交付；`examples/data/{tc002a_640,s07_1280}` 仅作本地离线大图测试） | 内部个人路径、内部分支/关联项目名 |
| `sdk_tests/` · `tools/` | 单元测试、模拟测试与打包/校验脚本（`package_sdk.py`、`verify_windows.ps1`） | — |
| `docs/` · `third_party/` | 客户文档与内部研发规范；`third_party/` 仅放私有头文件库（Asio / nlohmann_json） | 内部文档禁止打入客户交付包 |

---

## 3. 抽象层设计与单向依赖（禁止互相依赖）

```text
  L5 绑定与工具    examples/ · simulators/ · tools/          (仅调用 L0 纯 C ABI)
        ▼
  L4 插件适配实现  station/plugins/                          (实现 IMetricProviderV2；由 recipe_json.cpp 顶层装配)
        ▼
  L3 流程抽象编排  station/procedures/                       (依赖 L0、L1、L2 与自身 IMetricProviderV2)
        ▼
  L2 硬件与采集    station/instruments/ | station/acquisition/ (互不依赖；均仅依赖 L0)
        ▼
  L1 纯数学计算    analysis/ (IR_analysis)                   (仅依赖 L0，零硬件/IO)
        ▼
  L0 公共纯 C ABI  include/ (IR_status.h ← IR_types.h ← IR_plugin_abi.h / IR_station.h ← IR_device.h / IR_recipe.h)
```

### 核心抽象契约
- **`include/IR_*.h`**：外部唯一契约；`IR_plugin_abi.h` 仅依赖 `IR_status.h` + `IR_types.h`（不引入 `IR_station.h`）。
- **`AsioLink`**（`station/instruments/transport/`）：收口 TCP/串口收发与超时，6 类驱动仅管协议编解码。
- **`FrameSource` / `FrameQueue`**（`station/acquisition/`）：屏蔽采集后端差异，统一交付紧密 `[frame][row][col]` `uint16_t` RAW16 与真实帧戳。
- **`Measurement` / `DeviceLease`**（`station/procedures/` & `instruments/`）：保证单进程独占采集与仪器预约。
- **`MetricSpecV2` / `IMetricProviderV2`**（`station/procedures/`）：内置算子按域拆表登记；外部插件通过 `IMetricProviderV2` 抽象接入，由 `station/plugins/PluginMetricProvider` 实现并在组合根 `recipe_json.cpp` 按需注入。

### 严禁互相依赖红线（已通过 `#include` 审计确认）
1. `analysis/` **严禁**引用 `station/` 任何头文件。
2. `station/instruments/` 与 `station/acquisition/` **严禁**互相引用，且均**严禁**反向依赖 `station/procedures/` 或 `station/plugins/`。
3. 同层实现间（6 类仪器驱动之间、`raw_replay`/`directshow`/`v4l2` 之间）**严禁**互相引用。
4. `station/procedures/` **严禁**直接包含 `station/plugins/`（仅 C ABI 顶层装配入口 `recipe_json.cpp` 可装配注入 `PluginMetricProvider`）。
5. `include/` 内部保持严格有向无环图，**严禁**包含 `shared/`、`station/` 或 `third_party/`。

---

## 4. SOLID 与跨边界核心约束

- **SOLID 守则**：单一职责（驱动管协议、采集管 RAW16、分析管数学、流程管生命周期与回滚；`metrics_v2_*` 按域分文件，禁止回流千行巨石）；开闭原则（定制算子优先走 `IR_plugin_abi.h` 与 `plugin:<ns>.<name>`，结构体演进走 `struct_size`/`reserved`/`*Ex`）；里氏替换（后端与插件遵守一致的几何、有限浮点 `std::isfinite`、取消与内存预算契约）；接口隔离（离线分析仅链 `IR_analysis`，插件隔离 `IRPrepareCallbacksV1` 与 `IRHostCallbacksV1`）；依赖倒置（高层依赖纯 C ABI 与 `IMetricProviderV2`/`FrameSource` 抽象）。
- **跨 DLL 安全**：插件 `extern "C"` 回调须用 `ir_plugin_guard`（`IR_plugin_cxx.hpp`）兜底；宿主回调须 `extern "C" noexcept` 全捕获；读取插件 `errBuf` 前强制末尾写 `'\0'`；UTF-8 路径构造使用 `std::u8string`（禁用 `reinterpret_cast<const char8_t*>` 与废弃 `fs::u8path`）。
- **打包卫生（`tools/package_sdk.py`）**：`AGENTS.md`、`验证记录.md`、`docs/开发规范.md`、`docs/算法接入指南.md`、`docs/框架与二次开发实施计划.md` 不进客户包；进包文本严禁出现个人盘符、内部分支或关联项目名。

---

## 5. 构建与验收基线

```powershell
# 1. CMake windows Debug preset 配置、构建与 23 项 CTest
cmake --preset windows ; cmake --build --preset windows ; ctest --preset windows --output-on-failure

# 2. 全量 Windows Debug 验收（可选 -RunCom0ComHil 启用六路虚拟串口 HIL，发布前加 -RequireClean）
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify_windows.ps1 -PythonExe "<venv-python>" -RunCom0ComHil
```
