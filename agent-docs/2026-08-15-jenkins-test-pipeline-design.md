# bbtools-coroutine Jenkins 测试体系设计

- 日期：2026-08-15
- 状态：方案已确认，待实现
- 适用仓库：`bbtools-coroutine`
- 目标平台：Jenkins
- 关联现状：`.github/workflows/unit_test.yml`、`.github/workflows/memery_test_info.yml`

## 1. 背景与目标

当前仓库已有 CTest、Smoke、Reliability、性能压测、ASAN/UBSAN 和 Valgrind 入口，但测试职责、资源隔离、指标保存和告警链路没有统一闭环。现有 GitHub Actions 还存在以下问题：

- 性能基线写入后，PR Job 没有可靠消费，性能门禁长期处于 `PASS_NO_BASELINE`。
- 长测报告目录与上传路径不一致，运行结果可能无法留存。
- Sanitizer Job 依赖 `workflow_dispatch`，但主 Workflow 未声明该事件。
- 内存检测跨 Job 没有可靠传递构建产物。
- self-hosted runner 长测排队时间过长，短测和长测资源没有隔离。

本方案将测试体系统一迁移到 Jenkins，覆盖三个场景：

1. **定时 Smoke 测试**：验证编译、基础功能、正常退出和无 dump。
2. **PR 性能测试**：自动测量性能，保存完整指标，初期告警但不阻塞 PR；基线稳定后再启用门禁。
3. **持续疲劳测试**：低资源长期运行，观察内存、CPU、功能模块进展和进程生命周期。

Jenkins 是唯一测试执行平台。GitHub 继续承担源码托管、PR 管理和状态展示，不再作为测试流水线执行平台。

## 2. 非目标

本阶段不包含：

- 生产部署和自动发布；
- 自动修复代码或测试失败；
- 自动扩容 Jenkins 节点；
- 修改 coroutine 核心运行时逻辑；
- 直接接入 Prometheus/Alertmanager；
- 通过放宽 timeout 掩盖真实 hang、crash 或资源泄漏。

初期告警出口使用 Jenkins 构建结果和邮件。统一监控系统接入作为后续演进项。

## 3. 总体架构

```text
GitHub PR / main
        │
        │ Webhook / 定时 / 手动参数
        ▼
Jenkins Controller
  ├─ Pipeline、凭据、审批、构建记录
  ├─ GitHub PR 状态回写
  └─ 邮件告警
        │
        ├─ cpp-fast Agent
        │    ├─ Smoke
        │    └─ PR 性能测试
        │
        └─ cpp-soak Agent
             └─ 持续疲劳测试
```

### 3.1 Controller 职责

Controller 只负责控制面：

- 加载并执行 Pipeline；
- 管理 Job 参数和调度；
- 管理最小必要凭据；
- 保存构建记录、测试结果和归档索引；
- 回写 GitHub PR 状态；
- 发送邮件告警。

Controller 不承担 C++ 编译、长时间压测或资源采样。

### 3.2 Agent 职责

使用 Jenkins Label 区分资源边界：

- `cpp-fast`：短时构建、Smoke 和 PR 性能测试。
- `cpp-perf`：固定性能环境；可与 `cpp-fast` 分离，建议优先固定 CPU、编译器和并发策略。
- `cpp-soak`：持续疲劳测试独占节点或独占 executor。

性能测试不能随意落到不同硬件上比较。至少要记录 CPU、内存、编译器、CMake 参数、构建类型和线程数。

## 4. Job 划分

### 4.1 `bbtools-coroutine-smoke`

- 触发：Jenkins cron，默认每 30 分钟。
- 分支：`main`。
- 节点：`cpp-fast`。
- 运行：干净配置、编译、Smoke CTest 子集、退出检查。
- 失败：立即邮件，保留证据，不阻塞其他 Job。
- 支持：手动触发和参数覆盖。

默认调度表达式采用 Jenkins 的哈希分钟形式：

```text
H/30 * * * *
```

频率不写死在代码中，允许改成每 15 分钟、每小时或其他经过资源评估的周期。

### 4.2 `bbtools-coroutine-pr-performance`

- 触发：PR 创建、PR 更新、手动重跑。
- 节点：固定 `cpp-perf`，或带固定环境元数据的性能 Agent。
- 运行：PR commit 性能测试，并与同环境 main 基线比较。
- 初期：异常标记 `UNSTABLE`，发送邮件，不阻塞 PR。
- 稳定后：轻度退化告警，严重退化和 crash/timeout 阻塞 PR。

### 4.3 `bbtools-coroutine-soak`

- 触发：持续自动调度，也支持手动参数覆盖。
- 节点：独占 `cpp-soak` executor。
- 运行：单进程覆盖六个功能模块，6 小时一段，自动续跑。
- 资源：默认 1 个 Processer，低 CPU、低内存限制。
- 失败：邮件告警并保留现场；不吞掉失败，不以无限重试伪造绿色。

## 5. 定时 Smoke 设计

### 5.1 流程

```text
checkout main 固定 commit
  └─ 清理构建目录
      └─ CMake 配置
          └─ 编译测试目标
              └─ CTest Smoke 子集
                  └─ Test_smoke
                      └─ 进程退出和 dump 检查
                          └─ JUnit/日志归档
```

当前仓库实际注册 19 个 CTest。设计上应通过 CTest Label 将短 Smoke 与完整测试区分，避免依赖测试名称硬编码。候选 Smoke 集合包括：

- `Test_coroutine`；
- `Test_coroutine_exception`；
- `Test_poller`；
- `Test_chan`；
- `Test_comutex`；
- `Test_coevent`；
- `Test_copool`；
- `Test_smoke`。

最终 Label 清单以实现阶段读取当前 CMake 和测试行为后确定，不在本设计阶段虚构未注册的 Label。

### 5.2 通过条件

必须全部满足：

- CMake 配置成功；
- 编译退出码为 0；
- Smoke 测试退出码为 0；
- CTest 无失败、无 timeout；
- 无断言、`ERROR`、`Segmentation fault` 等错误信号；
- 无 core/dump 文件；
- 测试进程正常退出；
- JUnit/CTest 报告成功生成。

### 5.3 失败处理

Smoke 失败后：

1. 立即标记 Jenkins 构建失败；
2. 发送邮件；
3. 归档 CTest XML、stdout、stderr、构建参数、环境摘要和 dump 文件索引；
4. 不阻塞 PR 性能 Job 和持续疲劳 Job；
5. 默认不自动重试。

如果以后确认某个测试存在可接受的低概率 flaky，只允许对该测试设置有限次数、每次新进程重试，并在报告中保留首次失败证据。

## 6. PR 性能测试设计

### 6.1 测试模块

复用仓库已有 `unified_stress` 和相关脚本入口，覆盖：

- `comutex`；
- `corwmutex`；
- `cocond`；
- `chan`；
- `copool`；
- `coroutine`。

测试命令、持续时间、线程数和模块列表均参数化，避免在 Jenkinsfile 中散落常量。

### 6.2 流程

```text
checkout PR commit
  └─ 读取固定性能 Agent 元数据
      └─ 获取同环境最近成功的 main 基线
          └─ 使用相同参数构建 PR
              └─ 运行六个模块性能测试
                  └─ 解析结构化指标
                      └─ 与 main 基线比较
                          └─ 保存完整结果
                              └─ Jenkins 状态 + 邮件告警
```

推荐在同一性能 Agent 上先生成 main 基线，再测试 PR 候选，减少机器瞬时状态差异。若基线环境不一致，结果必须标记为不可比较，而不是静默显示通过。

### 6.3 指标保存

每次 PR 的完整结果长期保留；main 基线长期保留趋势。结构化结果至少包含：

```json
{
  "repository": "bbtools-coroutine",
  "commit": "<candidate commit>",
  "base_commit": "<main baseline commit>",
  "agent": "<agent identity>",
  "compiler": "<compiler version>",
  "build_type": "Release",
  "threads": 2,
  "duration_seconds": 45,
  "modules": {
    "comutex": {
      "ops_per_sec": 0,
      "lock_avg_us": 0,
      "errors": 0
    }
  },
  "verdict": "PASS|WARN|FAIL|NO_COMPARABLE_BASELINE"
}
```

同时保存：

- CMake 配置和构建类型；
- 编译器版本；
- CPU、内存和 Agent 标识；
- PR commit 与 main 基线 commit；
- 每模块吞吐、延迟、错误和 timeout；
- 原始日志；
- Markdown 汇总报告。

结构化指标和 main 趋势属于长期资产；原始 stdout/stderr 只作为归档证据，不应混入源码仓库。

### 6.4 初期处置策略

用户已确认：性能异常先告警，不阻塞 PR。

| 情况 | Jenkins 状态 | 初期是否阻塞 PR |
|---|---|---:|
| 无可比基线 | `UNSTABLE` | 否 |
| 轻度吞吐退化 | `UNSTABLE` | 否 |
| 严重吞吐退化 | `UNSTABLE` 或 `FAILURE` | 否 |
| crash、timeout、zero ops | `FAILURE` | 否，但必须告警 |
| 指标采集失败 | `FAILURE` | 否，但必须告警 |

积累稳定基线后再启用正式门禁：

- 轻度退化：告警；
- 严重退化：阻塞 PR；
- crash、timeout、zero ops：阻塞 PR；
- 无可比基线：不判定性能通过，需明确状态。

## 7. 持续疲劳测试设计

### 7.1 运行模型

采用低资源推荐档：

- 单进程；
- 六个模块同时运行；
- `--threads=1`，即 1 个 Processer；
- 6 小时一个运行段；
- 一段自然结束后归档，再自动启动下一段。

当前短测显示，单进程六模块比六进程并行更节省资源，也能覆盖模块交互。六进程并行保留为后续故障隔离方案，不作为首发模式。

### 7.2 默认参数

```text
SOAK_DURATION_SECONDS=21600
SOAK_THREADS=1
SOAK_CPU_QUOTA=1 CPU
SOAK_MEMORY_HIGH=128 MiB
SOAK_MEMORY_MAX=256 MiB
SOAK_RESOURCE_INTERVAL_SECONDS=10
SOAK_METRIC_INTERVAL_SECONDS=60
nice=10
```

这些是初始观测参数，不是未经 Agent 实测就永久固化的容量结论。所有参数支持 Jenkins 手动覆盖。首轮至少收集 7 天数据，再校准 CPU、RSS 和内存阈值。

### 7.3 采集内容

每 10 秒采集资源：

- 进程存活状态；
- CPU 使用率和累计 CPU 时间；
- RSS/PSS（权限允许时）；
- 虚拟内存；
- 线程数；
- OOM 或退出信号；
- core/dump 文件变化。

每 60 秒采集功能指标：

- `ops_total`；
- `errors`；
- 各模块进展；
- Lock/Cond/Channel 指标；
- CoPool 提交、完成、丢弃指标（已有字段不足时标记未覆盖）；
- Coroutine 生成和切换指标（已有字段不足时标记未覆盖）。

资源采样和功能指标必须带同一时间戳或可关联的 run ID，方便判断资源变化是否伴随功能退化。

### 7.4 分段续跑

自动续跑不是让同一个进程无限运行，而是：

```text
6 小时运行
  └─ 检查自然退出
      └─ 保存报告
          └─ 清理本段临时目录
              └─ 启动下一段
```

分段设计同时覆盖长期运行和反复 Start/Stop/初始化/销毁路径。任何一段异常都必须保留异常段的原始证据，不得用下一段成功覆盖失败。

### 7.5 通过与告警条件

每段疲劳测试必须满足：

- 运行达到目标时长；
- 进程正常退出；
- 无 core/dump；
- 无断言、`ERROR`、Segmentation fault；
- 六个模块均产生指标；
- `ops_total` 持续递增；
- `errors=0`；
- 不出现连续两个以上采样周期无进展；
- CPU 和内存不超过配置限制；
- 下一段能够正常启动。

初始资源告警：

- RSS 超过 `MemoryHigh`：告警；
- RSS 超过 `MemoryMax`：失败；
- RSS 连续多个小时单调增长：告警；
- 进程被 OOM kill：失败；
- 采样中断或指标缺失：失败或标记证据不完整。

单次 RSS 快照不能证明内存泄漏。泄漏判断必须基于固定间隔的时间序列，并关联 workload、重启点和功能指标。

## 8. 邮件告警

统一使用 Jenkins 构建结果和邮件：

| 场景 | 处理 |
|---|---|
| Smoke 编译失败 | 立即邮件 |
| Smoke timeout/hang | 立即邮件 |
| Smoke crash/dump | 立即邮件 |
| PR 性能退化 | 邮件，初期不阻塞 |
| PR 无可比基线 | 邮件提醒 |
| 疲劳模块无进展 | 立即邮件 |
| 疲劳 RSS 越界 | 立即邮件 |
| 疲劳 CPU 越界 | 立即邮件 |
| 疲劳 crash/OOM | 立即邮件 |

告警需要抑制重复噪声：相同故障指纹在 1 小时内只发送一次，故障恢复时发送恢复邮件，故障指纹变化时重新告警。故障指纹至少包含 Job、阶段、测试名、退出码和错误类别。

## 9. 迁移和下线计划

按以下顺序迁移：

1. 编写 Jenkins Pipeline、统一参数和报告 Schema；
2. Jenkins Smoke 单独跑通；
3. Jenkins PR 性能测试跑通，并验证基线写入和读取；
4. Jenkins Soak 完成一个完整 6 小时段；
5. 验证邮件告警，包括失败、重复抑制和恢复；
6. 验证 GitHub PR 状态回写；
7. 进行一个有限窗口的结果对照；
8. 关闭并移除 GitHub Actions 测试 Workflow；
9. 将 `docs/ci-guide.md` 更新为 Jenkins 真正流程。

迁移期间允许短暂双跑，仅用于结果对照。验收完成后禁止长期双轨。

## 10. 验收标准

### Smoke

- Jenkins 定时任务默认每 30 分钟触发；
- 频率可通过 Job 配置调整；
- 干净构建和 Smoke 测试通过；
- hang、timeout、crash、dump 能被识别；
- 失败邮件能收到；
- 失败不阻塞性能和疲劳 Job；
- CTest/JUnit/日志证据可下载。

### PR 性能

- PR 创建和更新能触发性能 Job；
- PR 与 main 使用可验证的同环境基线；
- 每次 PR 完整指标可长期查询；
- main 基线形成趋势；
- 无基线、不可比基线、指标缺失都有明确状态；
- 初期异常只告警、不阻塞 PR；
- crash、timeout 和退化结论有邮件证据。

### 持续疲劳

- 单进程六模块、1 个 Processer 模式可启动；
- 6 小时段能自然结束并自动续跑；
- CPU、RSS/PSS、线程和功能指标可关联查询；
- 无进展、错误、OOM、crash、dump 能告警；
- 资源参数可通过 Jenkins 调整；
- 异常段原始证据不会被后续成功段覆盖。

```text
ARCH_RISK: MEDIUM
```

主要风险：Jenkins Agent 真实资源容量尚需在目标节点测量；性能比较依赖硬件和编译环境一致；长测报告归档和邮件凭据需要单独验证；GitHub PR Webhook 与状态回写属于后续配置工作。
