# 三仓共用编码与维护规范

本规范供 `bbt-framework`、`bbtools-infra`、`bbtools-coroutine` 逐字复用。仓库现有实现风格与本规范冲突时，不做无关格式化或重命名；新增代码采用以下约定，明确的架构边界与安全约束优先。`.clang-format` 是 C++ 格式细则的唯一真源，本文件不展开其参数。

## 1. 命名

- 类型（`class`、`struct`、`enum`）使用 `PascalCase`；方法和函数使用 `PascalCase`。
- 成员名使用 `m_` 前缀加 `camelCase`；private/protected 方法使用 `_` 前缀加 `PascalCase`。
- 类型别名使用 `Ptr`、`SPtr`、`UPtr` 等后缀；枚举值遵循所在模块既有风格（现有代码同时有 `PascalCase` 与 `DEFAULT` 一类写法，不借规范迁移改名）。
- 命名空间按模块层级组织为 `bbt::{大模块}::{小模块}`，最多三层；禁止新增四层及以上嵌套，过深时重新划分模块。

## 2. 文件与目录组织

新增或整理后的通用布局：

```text
bbt/<module>/          源码与头文件按模块组织
 tests/<module>/       单元、集成与 live 测试；文件名 Test_<行为>.cc
examples/<name>/       每个示例独立目录，附 README
benchmarks/            仅在仓库需要基准或性能测试时建立
 docs/                 architecture/、decisions/、guides/、dependencies/
scripts/ scripts/ci/   构建、CI 与维护脚本
cmake/                 CMake 模块
.github/               workflows、PR 模板
```

- 根目录保留各仓入口文件（如 `AGENTS.md`、`README.md`、`CMakeLists.txt`、`.clang-format`、`.editorconfig`、`.gitignore`）。新目录按上表归类，不另建工具工作流或任务状态目录。
- 现有目录在本轮按已定迁移边界处理：coroutine 的 `unit_test/`、`example/`、`benchmark_test/` 已迁至 `tests/`、`examples/`、`benchmarks/`；framework 的 `framework/` 与 infra 的 `include/`、`src/` 暂不改名。不要把目标布局解释为要求一次性移动所有旧文件。
- 每个公开类原则上由独立 `.hpp` 声明；有非内联实现时配对 `.cc`。接口抽象放 `interface/` 子目录，纯虚基类使用 `I` 前缀；头文件用 `#pragma once`。目录层级与命名空间保持一致。
- include 顺序遵循仓库现有约定：对应/本文件所需头文件、标准库、项目公共头（如 `bbt/xxx`）、子模块头；避免无关 include。

## 3. 语言、架构与依赖

- 新增与迁移代码以 C++17 为基线，不得静默提升标准。遵循目标仓库的 CMake 配置；coroutine 现有 `-fno-rtti` 约束继续由其构建配置管理，不扩展为三仓新约束。
- 依赖方向固定为 `bbt-framework → bbtools-infra → bbtools-coroutine`；禁止反向依赖、环依赖和引用兄弟模块内部实现。核心接口不依赖具体扩展或业务；RPC/传输与注册中心客户端 adapter 由 infra 负责，framework/扩展负责通用集成与服务注册发现，应用负责业务接口和生成 Stub；不预建万能协议或生成器平台。调用方只链接所需模块，并固定已验收的跨仓版本。
- 评估复用顺序为标准库、Boost 1.90、仓内能力、确有必要的新依赖。标准库或 Boost 已有成熟能力时不重复造轮子；保留自研实现须记录可复核的语义、性能、平台或生命周期差异。新依赖须先确认，并评估活跃度、架构/平台、许可、安全、体积与退出路径；不得为少一个依赖重造复杂协议或安全组件。`bbtools-core` 已冻结，不新增 `bbt_core` 依赖。
- namespace 最多三层，形式为 `bbt::{大模块}::{小模块}`。公共头不暴露更高层模块的类型，也不让第三方类型、异常或线程模型渗入公共 API；无法隔离时说明兼容影响并独立审查。
- 模板用于类型安全和复用，避免技巧性、难理解或难诊断的元编程。adapter 优先采用 RAII、值语义/智能指针与 `std::chrono`，并明确类型安全错误和取消/生命周期。纯算法、类型工具、无状态小组件优先 header-only；有稳定 ABI、重状态、I/O、平台隔离或明显编译成本时，可保留编译单元并说明理由。
- adapter 负责 I/O、错误映射、取消/关闭及其资源生命周期；底层等待结束、逻辑关闭与物理回收不可混为一谈。依赖反转/回调不改变源码依赖方向，不建设万能协议平台。

## 4. API、错误与所有权

- 错误优先使用类型安全的状态/错误表达；保留现有模块内的返回码语义，适用时优先采用 `0` 成功、`-1` 错误、`1` 超时，不跨模块强行改写已公开契约。
- 不抛异常的接口声明 `noexcept`。状态用枚举显式表达，不依赖裸 `int` 常量；`volatile` 仅用于简单跨线程状态读取，不作为通用同步方案。平台属性复用已有 `BBTATTR_*` 宏，不重复造条件宏。
- 工厂 `Create()` 可返回项目约定的指针类型；按需用 `PrivateTag` 或 `protected` 构造限制直接构造。`SPtr` 对应 `std::shared_ptr`，`UPtr` 对应 `std::unique_ptr`；裸指针仅用于明确的非拥有关系或受控场景。
- 明确每个对象、回调与异步事件的所有权和寿命；事件/回调跨越异步边界时，确需共享寿命则使用 `shared_ptr`。不为微观性能机械替换智能指针，所有权理由优先于指针形式。

## 5. 注释

- 注释以中文为主，英文术语、标识符、API 名和错误原文保留原样。
- 解释 WHY，不复述代码已经表达的 WHAT。涉及线程/协程交错、挂起/唤醒、取消、事件顺序、所有权或资源关闭时，写明必须成立的时序/生命周期约束；复杂机制用多行注释说明设计意图与事件流。
- `TODO` 写明已知的改进方向。公开头文件开头用一句话说明职责。
- 不在注释或长期文档里留 Issue/PR 号、审查/阶段代号、契约行号或临时版本标记；过程关联放在 commit、PR 或 Issue 中。允许保留协议/RFC 编号和指向稳定 ADR 文件的引用，细则见 `forbidden-process-noise.md`。

## 6. 测试

- 统一使用 Boost.Test；每个测试源文件构建为独立可执行目标，使用 `BOOST_AUTO_TEST_SUITE` / `BOOST_AUTO_TEST_CASE`。进程隔离场景可按 case 注册到 CTest（例如传入 `--run_test=Suite/Case`），但不因此另建第二套测试框架。
- 测试文件命名为 `tests/<module>/Test_<行为>.cc`；CTest 名称统一为 `<module>.<行为>`。
- 每个测试按实际性质打一个或多个 label，label 名称只能使用：

  | label | 用途 |
  |---|---|
  | `smoke` | 构建后快速确认基本可用性的冒烟用例 |
  | `unit` | 模块内单元行为与边界 |
  | `integration` | 多模块或真实组件协作，但不依赖线上外部服务 |
  | `live` | 需要外部服务/环境的真实集成用例 |
  | `perf` | 基准、性能回归或疲劳性能用例 |

- 新测试须在对应 `CMakeLists.txt` 添加 target 并用 `add_test()` 注册到 CTest；核对构建开关、注册集合及 CI 执行路径。测试覆盖正常路径、竞争/并发、超时、重入、状态清理与错误边界等相关情形。
- 并发/生命周期用例先证明已进入目标阶段再注入故障或关闭，检查结果和资源收口；用同步原语（如 coroutine 的 `CountDownLatch`）建立时序，固定 `sleep`、用例名或通过数量不能替代阶段证据。fixture 只补时序控制，不能替代真实后端集成。
- 新增行为要有可运行验证；CI 全绿只证明实际执行范围，不能替代新路径验收。权限、环境或依赖不足时明确标为 `BLOCKED`，不以 stub、跳过断言或放宽检查伪装完成。三仓统一本地边界见 `agents-common.md`：冒烟、本次功能与直接耦合测试，本地不跑全量。
- 测试优先使用自有进程与动态端口；共享服务须说明复用/互斥及清理边界，不终止他人进程、不覆盖共享前缀。无法隔离时先报告阻塞。

## 7. 示例

- 每个示例放在 `examples/<name>/`，附 `README.md`，说明演示内容、运行方式和期望输出。
- 示例只展示用法；断言和验收逻辑写进 `tests/`，不在示例中用断言充当测试。
- CI 编译全部示例；依赖外部服务的示例只编译、不在普通 CI 中运行。

## 8. 文档分类与维护

| 位置 | 内容与维护规则 |
|---|---|
| `README.md` | 项目是什么、当前状态、最短构建/使用路径、文档地图；不超过 200 行，不放 API 全表或流程正文。 |
| `docs/architecture/` | 稳定架构设计与契约；记录目标契约，变更随 PR 审查。coroutine 运行时契约迁入此处。 |
| `docs/decisions/NNNN-<slug>.md` | ADR；编号唯一且递增。合入后不改正文，只追加「已被 NNNN 取代」。稳定引用使用相对路径。 |
| `docs/guides/` | development、ci、performance、api-reference 等操作指南；一个主题一篇，随代码在同一 PR 更新。CI 指南唯一落点为 `docs/guides/ci.md`。 |
| `docs/dependencies/` | 第三方依赖来源与证明材料（infra）。 |
| 仓库外 / PR / Issue / CI artifact | 运行证据、审查报告、阶段计划和会话记录；不作为长期文档提交。perf 基线仍放 `perf-baseline` 分支。 |

- 一事一处：一个事实只在一篇文档写全，其他文档链接到它。README 只做入口，不复制 API 或流程正文。
- 标题和文件名写主题，不写 Issue/PR 号、阶段代号、临时版本代号或日期；ADR 编号是唯一的编号例外。
- 文档与对应代码在同一 PR 更新；被取代的文档直接删除，历史交给 Git。修改/删除历史链接前确认其替代关系；文档改动运行 `git diff --check` 并核对引用路径和命令。
- 长期资产按未来维护/决策需要判断并随相关变更维护；运行证据、原始日志、采样、构建/覆盖率产物、缓存、会话、凭据和私有路径不入仓。不要仅因产物由 AI 生成就另建目录；先更新已有权威文档，检查 `.gitignore`，被忽略文件不能算交付。
- docs-check 至少检查相对链接有效、文件名无过程编号、README 不超过 200 行。

## 9. 格式、构建与警告

- C++ 格式参数由 `.clang-format` 定义；`.editorconfig` 统一 UTF-8、LF、4 空格缩进和文件末尾换行，Makefile/YAML 例外按需设置。
- CI 只对 PR 改动行运行 `clang-format-diff`；不强制格式化存量代码。全仓格式化须单独决策和提交，不夹带在功能变更中。
- 使用 CMake；不得引入新编译器警告。新模块/测试需接入相应 `CMakeLists.txt`。本地构建并发和验证边界见 `agents-common.md`，禁止无参数并行构建。

## 10. CI 骨架

- `changes` 在本仓 workflow 内完成路径分类；不引入共享 classify/result callee 链。
- `build-test` 在 `ubuntu-24.04` 上执行 Release 构建及 `ctest -L "smoke|unit"`。
- 仓库专属 job：coroutine 的 perf；infra 的 live（Redis/Mongo 分开成 job）；framework 的 consume 验证。
- `result` 汇总并作为唯一 required check。docs-only PR 只运行 `docs-check`。
- 普通 CI 固定使用 `ubuntu-24.04`，不使用浮动 `ubuntu-latest`；ARC 仅用于 perf 和 memcheck。
- Workflow 只编排，执行逻辑放 `scripts/ci/`；单个 workflow 文件不超过 300 行。
- CI 变更、发布流程变更及系统级安装均需事先确认；分支保护须按授权流程修改。
