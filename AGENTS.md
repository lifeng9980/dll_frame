# AGENTS.md

本文件为在 `dll_frame`（`IR SDK`）仓库工作的编程代理提供精炼的架构上下文、收束边界、SOLID 约束与构建验证基线。详细规范与客户文档见：

- [开发规范.md](docs/开发规范.md) — 三层权责、标准差/掩码口径与开发规范（内部）
- [算法接入指南.md](docs/算法接入指南.md) — 算法规格卡与内置/插件算子接入（内部）
- [SDK.md](SDK.md) / [客户接入说明.md](docs/客户接入说明.md) / [测试人员配方指南.md](docs/测试人员配方指南.md) — 客户交付文档
- [验证记录.md](验证记录.md) — 各里程碑构建、HIL 与打包校验记录（内部）

---

## 1. 项目定位与收束边界

`IR SDK`（当前版本 `1.8.0`，C++20，Windows x64 / Linux x86_64）是面向红外探测器自动化测试与图像分析的**纯 C ABI 双动态库框架**，由 `IR_analysis` 与 `IR_station` 组成，支持声明式 JSON 配方（Schema v1 / v2）及外部动态算子插件（`IR_plugin_abi.h`）。

### 硬性收束边界（Non-Goals）
- **不做 UI/HMI 与工单业务**：界面交互、产品良率阈值和工单编排留在 SDK 之外的上位机，SDK 只暴露纯 C ABI 与 CLI 入口。
- **不拆分微库**：除 `IR_analysis` 与 `IR_station`（以及外部客户按 `IR_plugin_abi.h` 自建的指标插件）外，不为单个仪器或内置算法额外拆分 DLL/SO。
- **不引入重型运行依赖**：核心双库保持**零 Qt、零 OpenCV、零第三方运行时动态库**。`Asio 1.36.0` 与 `nlohmann/json 3.12.0` 仅作私有编译期头文件依赖，严禁进入 `include/` 公共头。
- **不破坏既有兼容语义**：`IR_RunNoise` 与 Schema v1/v2 `temporal_noise` 固定使用逐像元样本标准差（分母 `N - 1`）；GJB 总体标准差（分母 `N`）与新演进能力必须通过独立指标名、插件或 `*Ex` 新入口提供。

---

## 2. 分层架构与单向依赖

```text
  L5 绑定与工具    examples/ (C, C++, ir_run_recipe, Python, C#) · simulators/ · tools/
        │  仅调用纯 C ABI（除内置 CLI 工具直用 nlohmann_json 做路径改写）
        ▼
  L4 流程编排层    station/procedures/ (measurement, recipe_config, recipe_json, recipe_v2,
        │             recipe_metrics, metric_registry_v2, metrics_v2_{basic,standard,spectral}, save_raw)
        ├──────────────────────────────┐
        ▼                              ▼
  L3 插件沙箱层    station/plugins/      L2 硬件与传输层
    (plugin_manager,                     station/instruments/ (transport/asio_link, device_lease, 6类驱动)
     plugin_host_adapter)                station/acquisition/ (FrameSource, FrameQueue, raw_replay/directshow/v4l2)
        │                              │
        └──────────────┬───────────────┘
                       ▼
  L1 无状态计算层  IR_analysis (analysis/*.cpp: 统计, ROI, 3D噪声, GB/GJB, 可组合算子, 频域/FFT2D, 闪烁)
                       │
                       ▼
  L0 公共纯 C ABI  include/ (IR_status.h, IR_analysis.h, IR_device.h, IR_station.h, IR_recipe.h, IR_plugin_abi.h)
```

| 层级 / 目录 | 核心文件 | 单一职责与边界 |
|---|---|---|
| `include/` | `IR_*.h` | 纯 C99/C11 稳定 ABI 契约；固定宽度整数、UTF-8 文本、`IRStatus` 错误码、调用方分配或明确回调释放 |
| `analysis/` | `IR_analysis` | 无状态纯数学与图像算子，线程安全，零硬件、零文件/串口 IO |
| `station/instruments/` | `asio_link.*`, `device_api.cpp`, 6 类仪器驱动 | 驱动仅负责型号协议编解码与语义校验；TCP/串口收发与超时统一收口于 `AsioLink`；`device_lease.h` 管理独占租约 |
| `station/acquisition/` | `frame_source.*`, `frame_queue.*`, 平台后端 | 对上交付紧密 `[frame][row][col]` `uint16_t` RAW16 与真实帧戳；禁止在后端做 RGB/YUV 转换或伪造时间戳 |
| `station/plugins/` | `plugin_manager.*`, `plugin_host_adapter.h` | `LoadLibraryExW`/`dlopen` 加载、ABI/版本预检、`HostCallbackBridge` 槽位沙箱与 `max_inflight_bytes` 记账、SHA-256 溯源 |
| `station/procedures/` | `recipe_config.*`, `recipe_json.cpp`, `recipe_v2.cpp`, `metrics_v2_*.cpp` | 工位/配方解析、预检（`IR_CheckRecipeJson`）、流程编排（`IR_RunRecipeJson`/`Ex`）、`step_records` 采集与安全回滚 |

---

## 3. SOLID 原则架构约束

- **S — 单一职责（SRP）**：
  - `IR_analysis` 只算数值；仪器驱动只管协议；`FrameSource` 只产 RAW16 帧；流程层只负责生命周期、仪器协调、安全收尾与归档。
  - Schema v2 内置算子已按域拆分为 `metrics_v2_basic.cpp`、`metrics_v2_standard.cpp`、`metrics_v2_spectral.cpp`，严禁再回流成千行单文件。
- **O — 开闭原则（OCP）**：
  - 客户/项目定制算子优先通过外部动态插件（`IR_plugin_abi.h` + `station.plugins` + `plugin:<ns>.<name>`）扩展，避免重编核心 `IR_station`。
  - 新增内置 v2 算子通过 `metric_registry_v2.h` 的 `MetricSpecV2` 注册表接入；C ABI 结构体仅通过 `struct_size` / `abi_version` / `reserved` 与 `*Ex` 函数扩展。
- **L — 里氏替换（LSP）**：
  - 所有 `FrameSource` 后端（`raw_replay` / `directshow` / `v4l2`）须满足一致的几何报告、紧密像素布局、异步取消响应与短读连续前缀（`completed`）语义。
  - 外部插件算子与内置 `MetricSpecV2` 须遵守一致的两阶段 `prepare`/`execute` 校验、有限浮点数（`std::isfinite`）与内存预算契约。
- **I — 接口隔离（ISP）**：
  - 仅做离线算法计算方只需链接 `IR_analysis`（依赖 `IR_status.h` + `IR_analysis.h`）。
  - 插件接口严格区分静态预检回调 `IRPrepareCallbacksV1` 与运行期执行回调 `IRHostCallbacksV1`，插件仅能读写自身在预检阶段声明/绑定的槽位。
- **D — 依赖倒置（DIP）**：
  - 上层调用方与测试仅依赖 `include/IR_*.h` 纯 C 接口；编排层依赖 `FrameSource` 与算子注册表抽象，不耦合具体硬件总线或算法实现。
  - **收束红线**：禁止继续扩大 `station/procedures` 与 `station/plugins` 的双向包含耦合，新代码应向抽象提供者接口收束，避免在解析阶段硬编码具体 OS 装载实现。

---

## 4. 核心不变量与开发清单

### 运行与边界不变量
1. **线程与取消语义**：`Measurement` 持有独占采集与仪器运行期；跨线程 `IR_RequestStop` 必须在同步轮询间隙协作中断；`IR_CheckRecipeJson` 等预检失败不得打开硬件，也不得污染 `IRRecipeResult::cleanup_status`。
2. **跨 DLL C ABI 边界**：
   - 插件导出的 `extern "C"` 回调必须捕获全部 C++ 异常（使用 `ir_plugin_guard`），宿主侧向插件暴露的回调表入口同样必须为 `extern "C" noexcept` 并全路径兜底异常处理。
   - 宿主读取外部插件写入的错误缓冲区（如 `errBuf`）前必须强制在末尾写入 `'\0'`，并校验插件产生的标量满足 `std::isfinite`。
   - UTF-8 字符串转 `std::filesystem::path` 须构造 `std::u8string`（禁止 `reinterpret_cast<const char8_t*>` 严格别名未定义行为或已废弃的 `std::filesystem::u8path`）。
3. **打包与仓库卫生（`tools/package_sdk.py`）**：
   - 本文件（`AGENTS.md`）、`验证记录.md`、`docs/开发规范.md`、`docs/算法接入指南.md`、`docs/框架与二次开发实施计划.md` 为内部研发文件，**不进入客户交付包**。
   - 会进包的文本（`include/`、`docs/客户*.md`、`examples/`、`simulators/`、`SDK.md`）严禁出现个人盘符路径或内部分支/内部关联项目名称。

### 扩展点速查
- **新内置标准算法**：`analysis/`（先补算法规格卡）→ 在 `include/IR_analysis.h` 声明 → 必要时登记到 `metrics_v2_*.cpp` 与 `recipe.v2.schema.json`。
- **新外部算子插件**：独立实现 `include/IR_plugin_abi.h`（`IR_PLUGIN_QUERY_V1_SYMBOL`）→ 通过 `station.json` 的 `plugins` 与配方 `"method": "plugin:<ns>.<name>"` 接入。
- **新仪器 / 相机后端**：仪器放 `station/instruments/<kind>/`（复用 `AsioLink`）；相机实现 `FrameSource` 放 `station/acquisition/<backend>/` → 更新 `CMakeLists.txt` 与 `docs/设备支持矩阵.md`。

---

## 5. 构建与验收基线

```powershell
# 1. CMake windows preset 配置、构建与测试（含 23 项 CTest，含 plugin_abi_c_check / plugin_abi / recipe_v2）
cmake --preset windows
cmake --build --preset windows
ctest --preset windows --output-on-failure

# 2. 全量 Windows 14 步端到端验收（构建、dumpbin、VERSIONINFO 1.8.0.0、打包卫生、中文空格解包、C/C++/Python/C# 示例、故障注入、Schema）
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify_windows.ps1 -PythonExe "<venv-python>"

# 3. 启用六路 com0com 并发 HIL（首次需管理员运行 setup_com0com_pairs.ps1 -EmitSerialMap .\实验\hil_serial_map.json）
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\verify_windows.ps1 -PythonExe "<venv-python>" -RunCom0ComHil
```

> 正式双平台发布前，应在干净 Git 工作树使用 `verify_windows.ps1 -RequireClean` 与 Linux 安装前缀经 `tools/package_sdk.py --require-clean` 合包校验。

---

## 6. 当前架构待收敛项（v1.8.0 审查结论）

> 前序 `P0`（插件同槽位别名原地破坏/UAF）与全部 `P1` 跨边界健壮性/HIL 收尾项已清零并经全量 `23/23` CTest 与六路 `com0com` HIL 验证通过。

后续演进优先按以下剩余 **SOLID 结构收束项（`P2`）** 推进：
1. **编排器与解析器拆分（SRP / OCP）**：拆分 `station/procedures/recipe_v2.cpp` 过长编排/解析函数（如 `runRecipeV2`、`parseSharedControlOrCaptureOrFlushOrAnalyze`），并抽离 `recipe_json.cpp` 与 `recipe_v2.cpp` 共用的进度/事件/收尾骨架。
2. **解除 `procedures <-> plugins` 循环耦合（DIP）**：在 `station/procedures/` 定义抽象 `IMetricProvider` 接口由 `station/plugins/` 实现，支持向 `parseRecipeV2()` 注入预加载提供者，消除 `--check` + `--run` 双次插件装载与哈希计算。
3. **精简插件 C ABI 头依赖（ISP / DIP）**：将 `IRGeometry` / `IRFrameStamp` 等公共最小 POD 值类型从 `IR_station.h` 抽离至独立 `IR_types.h`，并将 C++ 辅助模板 `ir_plugin_guard` 移至可选 `IR_plugin_cxx.hpp`。
