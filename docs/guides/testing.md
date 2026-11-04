# bbtools-coroutine 测试规范

本仓测试分层、执行入口与必要性判定的唯一落点文档；与 `docs/conventions.md` §6
（测试代码约定）互补：conventions 规定测试怎么写，本文规定测试什么时候跑、
在哪跑、由什么门禁判定。三仓统一口径，逐仓落盘本文不维护第二份发散副本。

## 1. 测试分层

`unit`/`integration`/`live`/`perf` 表达测试**性质**（label），
`smoke`/`focused`/`full` 表达**执行策略**（profile），两层不混用。

| 层级 | label/profile | 范围 | 时机 |
|---|---|---|---|
| 冒烟 | `smoke` | 构建后快速确认基本可用性；秒级-分钟级 | 每次代码 PR + main 集成 |
| 聚焦 | focused（测试名正则 / label 表达式） | 本次改动行为 + 直接耦合行为 + 对应回归 | 本地开发；普通 PR 由 changes 映射的必要 focused 步进 CI |
| 完整回归 | `full` | 全部 unit + integration（默认排除 live/perf） | 本地手动、发布 |
| 真实服务 | `live` | 真实外部服务（如 Redis） | 发布 Gate |
| 性能 | `perf` | 基线、压力、疲劳 | 性能相关改动、定时、发布 |
| 诊断 | 非普通 profile | memcheck、stack 预算诊断 | 定时、发布、手动 |

## 2. 统一执行入口

所有 profile 走 `scripts/ci/run_test_profile.sh`：

```
run_test_profile.sh --build-dir <dir> --profile <smoke|focused|full> [选项]
```

| profile | 选集 |
|---|---|
| `smoke` | `-L smoke` |
| `focused` | 必须给 `--regex` 或 `--label-expr`（二者都给取交集），否则 exit 2 |
| `full` | 默认 `-LE 'live\|perf'`；`--include-labels live` 可从排除集放回 |

执行契约（fail-closed）：查询注册集 → 选集非空（`--require-count`/`--require-file`
可加严）→ `ctest -j1 --no-tests=error --output-on-failure` → JUnit →
校验**注册集=实跑集**，skipped/Not Run/failure/error 一律失败。
主动 SKIP（如缺 Redis 的 live 用例）在选集阶段（`-LE live`）排除，不靠事后容忍。

## 3. 执行时机

| 路径 | 内容 |
|---|---|
| 普通 PR CI（`编译 & 单元测试`） | 一次 Release 构建 + 全量 smoke 标签集 + **必要 focused**：`changes` 把变更路径经 `changed_files.py` 内 `FOCUSED_MAP`（路径前缀→ctest 测试名正则，覆盖全部 `bbt/**` 与 `tests/**`）映射为 focused 正则，非空时同一构建树追加 `--profile focused --regex <affected>`；**不自动重试**，无独立 Test_smoke/Test_reliability step |
| 本地默认验证 | smoke + 本次改动相关 focused（`--regex`/`--label-expr`）；本地不跑全量 |
| 完整回归 | `run_test_profile.sh --profile full`；发布 Gate（release.yml）跑全量 CTest（含 `Test_reliability` 等 integration 用例） |
| 性能 | perf 位触发时 `性能回归检查`（同 runner base/head）；发布 Gate 完整性能判定 |

CTest 注册集 = 构建目标 = 实跑集；必跑项 0 skipped、0 Not Run、0 缺失二进制。
flaky 失败保留证据修复，不用重跑掩盖。

## 4. 必要性判定

新增测试必须回答：保护哪条公共行为/错误语义/状态转换/资源生命周期；输入或事件
序列；断言观察的结果/状态/副作用；能区分哪种具体错误。

**必须新增或修改**：修复真实 bug；新增公共 API/错误语义/状态转换；改动取消、
超时、关闭、回收、并发、重入；改协议/wire/跨模块边界；改测试选择逻辑。

**不新增**：只为凑数；重复已有输入断言；只断言 mock 被调用；只断言进程返回 0；
复制 fixture；无新风险的复杂代码。

## 5. 代码约定（详见 conventions §6）

- Boost.Test + CTest，不引入第二套框架；文件 `tests/<module>/Test_<行为>.cc`，
  CTest 名 `<module>.<行为>`，label ∈ {smoke,unit,integration,live,perf}。
- 每个 smoke 用例必须有对应构建 target；进程隔离场景按 case/mode 注册多条 CTest。
- 并发/生命周期用例先证明进入目标阶段再注入故障或关闭。
- 删测试前确认其覆盖的唯一风险已被接管或明确记录。

## 6. 已知 flaky 与有界重试

两项既有不稳定测试（追踪 issue #427，修好后删除余量）：

- `coroutine.combined_wait`：同一 fd 同时申请 READABLE/WRITEABLE 时，唤醒掩码在两条投递
  路径上的编号不一致（`EventOpt` 的 READABLE/WRITEABLE 与 `PollEventType` 同名位值互换），
  约 8% 概率把可读就绪判成 `FdWriteable`。本地 120 轮 ctest 复现 8～15 次。
- `coroutine.poller`：`t_poller_timerout_event_multi_thread` 偶发 `std::length_error`
  崩溃（疑似未定义行为）；本地直跑 40 轮、本地 full 123/123 均未复现，只在 CI 负载下出现。

处理约定：CI 的 focused 步骤只对上述两项做**有界重试**（上限 3 次，且首轮失败项必须全部
属于它们；出现任何其它失败或没有失败清单，立即判红）。确定性失败会在 3 次重试中持续失败，
因此不会因这条余量漏过回归。不允许把它扩大成通用重试——通用重试会掩盖新引入的 flakiness，
正是本次收敛要去掉的东西。

根治按 issue #427 单独推进：统一掩码编号、查明 `length_error` 的来源，属并发/内存安全关键
改动，需独立审查后单独提交，不在 CI 改动里夹带。
