# AGENTS.md — bbtools-coroutine

定位：基于 boost.context 的有栈 C++ 协程库，仅 Linux；负责协程调度、并发原语、等待/唤醒/取消竞争、Hook 与运行时对象寿命；不承载业务逻辑、网络协议与跨机器语义（属 infra/framework）。

<!-- bbtools:common:begin -->
依赖方向固定为 `bbt-framework → bbtools-infra → bbtools-coroutine`，禁止反向依赖、环依赖和引用兄弟模块内部实现。
新 worktree/clone 不会带入未提交规范；开工先核对实际规范快照与依赖版本。
跨仓公共契约以契约所在仓文档为真源，变更由用户拍板；并行写任务使用独立工作区、索引、build 和依赖目录，保留他人已有改动。
统一以 C++17 为基线；优先标准库与 Boost 1.90，模板适度，namespace 不超过三层（`bbt::{大模块}::{小模块}`）。
设计、编码、Review 与用例设计的共用判断规范见 `bbtools-pub`（https://github.com/yqm-307/bbtools-pub，以其 `main` 已合入内容为准；本地并列检出 `../bbtools-pub` 可读，但不读其未提交改动），从其 `README.md` 只走当前职责的路径，不全量预载；与本仓 AGENTS 或 `docs/` 冲突时以本仓为准，并在交付中报告差异。
本地构建并发固定为 2；使用共享队列 `flock /tmp/bbt-build.lock`，构建显式 `--parallel 2`，configure 使用 ccache；测试用 `ctest -j1`。
本地验证必须覆盖冒烟、本次功能及直接耦合测试；本地不跑全量，完整测试走 PR CI。
只改获授权范围，不覆盖他人已有改动；Issue/任务存在不等于实施授权。
须先获用户授权：commit、push、开 PR、付费调用、部署、破坏性操作、新依赖、CI/发布变更、公共接口或数据布局破坏性变更、系统安装或运行基础设施操作。任务会产生改动时，开工时一次问清是否授权 commit/push/开 PR（无法询问时按未授权处理），授权覆盖整段任务、不逐步再问；未授权则停在 commit 前，交付改动与验证结果。
提交标题使用 `<type>(<scope>): <简洁中文说明>`，单次提交不混无关变更；目标测试通过且无新增警告后才允许 commit。交付前检查无意外改动并报告未覆盖项。Agent author 固定写法：`git commit --author="agent <agent@users.noreply.github.com>"`。
不直推 `main`、不自行合并或发布；受保护文件（含 `AGENTS.md`）、公共契约、权限，以及等待/唤醒/取消竞争、调度状态机、对象寿命与资源收口路径的改动须独立审查，其余按风险自审并跑相关测试；本仓文档另列须独立审查的类别从其规定。远端写入后回读确认目标对象。
文档一事一处、代码与对应文档同一 PR 更新；README 只保留状态与入口；ADR 使用唯一递增编号，合入后只追加取代关系。
注释说明 WHY，不重复 WHAT；必须保留时序与所有权约束，公开头文件开头说明职责；不留 Issue/PR、审查/阶段代号、契约行号或临时版本号。
<!-- bbtools:common:end -->

## 本仓块

- 跨仓架构基调真源：`bbt-framework/docs/architecture/distributed-framework-baseline.md`；本地并列检出时对应 `../bbt-framework/docs/architecture/distributed-framework-baseline.md`。
- 本仓细则（命名/目录/API/所有权/注释/测试/示例/文档/CI）：`docs/conventions.md`；格式以 `.clang-format`、`.editorconfig` 为真源；编码冲突时保持该模块既有风格一致性。
- 对 core 固定版本后的集成验证按 `docs/guides/development.md` 验证阶梯复验，不把上游通过当成本仓通过；infra 消费形态未实现前不声称已联调。

### 真源入口

- 契约：`docs/architecture/core-runtime-contract.md`（唯一契约真源；记录目标契约，不等于当前实现；实现与契约冲突时显式记录并补回归，不用当前行为默默改写契约）。
- API/用法：`docs/guides/api-reference.md`、`docs/guides/user-guide.md`；写**调用本库**的代码先读 `.github/skills/bbtools-coroutine/SKILL.md`。
- 流程：`docs/guides/development.md`（开发、验证阶梯、发布 Gate，选既有命令不另起流程；CI 结果、压测完整性与发布 Gate 是事实依据）。
- CI 使用：`docs/guides/ci.md`；疲劳/基准测试本地路径：`.github/skills/managing-fatigue-tests/SKILL.md`。

### 本仓特有门禁

- GitHub Issue/PR/Review 是项目需求、决策、进度与验收的消息真源：动手前读对应线程，无法访问时明确说明；发现消息、契约、实现或测试冲突时显式记录，不静默选择。用户最新明确指令优先于历史 Issue/PR。
- 在用户交办的任务范围内：用户明确要求实施，或所涉 Issue 已有方案共识时，直接实现；改公共 API 或契约、Issue 尚无共识且用户未明确要求直接实施时，先走「现状分析 → 方案共识」再实现。只在疑问会改变做法且自己查不到时才问。
- 性能优化先读 `docs/guides/performance.md`，按其基线准备、指标口径与快速抽查入口执行；抽查不替代功能验证与发布 Gate；缺数据、超时、错误如实报告，不自动补采或扩大测试。
- 不假设链接库或系统工具可用（先查 `CMakeLists.txt` 与邻近文件）；不修改 `.env`、密钥或凭据；不终止他人进程。
- 顶层目录按三仓同构目标布局：`bbt/`、`tests/`、`examples/`、`benchmarks/`、`docs/`、`scripts/`、`cmake/`、`.github/`，另有本仓 `debug/`（`NEED_DEBUG` 调试程序）；不新增顶层目录；构建产物只进已忽略的 `build*/`；运行证据、原始日志不入仓（perf 基线见 `perf-baseline` 分支）。
