# Processer 动态扩缩容：本地验收摘要

## 版本与结论边界

- 基线：`1e9f0e8c25de85dde003def65bb58b90bdac0e0b`；分支：`feat/processer-dynamic-scaling`。
- 已测产品候选：16 个源码、测试及契约/API 文件，指纹 `12d25e5ac8debb952217fe1911808a8365f57790f56ecc83352c29aa31df7d2b`。本摘要不参与产品指纹。
- **本地实现、功能回归、本轮可比性能及独立审查已完成。** 完整候选审查与最后四文件增量复审均 PASS，无未修复 P0/P1；交付边界为未提交的本地候选，不含远端集成。
- 未提交、未推送、未创建 PR、未运行远端 CI 或发布；本摘要不表示远端门禁通过。

## 实现及关键修复

支持手动与配置阈值驱动的自动扩缩容；手动请求切 Manual，显式恢复 Auto；物理线程协作退役，TLS 析构完成后由锁外 reaper join。Bound/Failed/Detaching driver 均 pin；NO_LOOP 无隐藏调度线程。parked 协程不因 worker 退役被展开或销毁。

已复现并回归：准备阶段分配失败原子回滚、TLS 析构阻塞不妨碍事件推进、Profiler 批次登记不遗漏、新增 worker 能分担原有连续短任务。最后一项修复只在解析后 `min < max` 时绕过旧单次 Resume 停顿阈值；固定范围保留旧策略，admission/drain/driver 门禁不变。

## 实际功能验证

Release 与 PROFILE=ON 各自独立构建，C++17、`-fno-rtti`；相关源 `-Wall -Werror` 语法检查、`git diff --check` 均退出 0。

| 验证 | 结果 |
|---|---|
| Release 扩缩容 CTest | 22/22 |
| Smoke | 8/8 |
| 直接耦合 CTest | 25/25 |
| PROFILE 扩缩容 CTest | 23/23 |
| PROFILE 高风险 9 项各重复 5 次 | 45/45 实际执行 |
| 短任务分摊回归：Release / PROFILE 各重复 10 次 | 20/20 实际执行 |
| PROFILE 扩容分配故障扫描 | 25 个实际命中点均回滚且可重试；首次未注入成功不计覆盖 |

短任务分摊回归先实跑 RED（退出 8，新增 worker 执行数为 0），修后通过。独立原探针 3/3 通过：1200 个 200µs 任务全部完成，新增 worker 分担 899–900 个，排空 60.417–60.561ms；修前同探针约 241ms、分担数 0。持久回归逐任务索引检查恰好一次，不只检查总计数。

## 同环境性能比较

旧版从独立基线 worktree 构建，`ldd` 核对 DSO 来源；共用 benchmark 源，Release/O3、PROFILE=OFF，同 6 核亲和、串行轮换配置，移除 `LD_LIBRARY_PATH`。每工况/配置 3 个独立进程，每进程 1 次预热和 3 个测量批次：**36 个进程、108 个计入批次全部退出 0**，各任务索引恰好一次。

排空时间中位数（ms）：

| 工况 | 旧版固定 4 | 候选固定 4 | 候选固定 1 | 自动 1→4 |
|---|---:|---:|---:|---:|
| 短突发：2000×30µs | 16.232 | 16.226 | 61.718 | 24.213 |
| 持续：20000×300µs | 1509.541 | 1508.732 | 6027.729 | 1519.200 |
| 纯调度：10000×0µs | 88.832 | 67.960 | 13.405 | 37.879 |

- 固定 4 相对旧版吞吐变化：短突发 +0.037%、持续 +0.054%、纯调度 +30.713%，本轮未超过约 10% 稳态回归门槛；微小变化与纯调度结果不宣称为稳定提速。
- 持续负载自动模式排空比候选固定 4 慢 0.694%，相对固定 1 加速 3.968 倍。
- 自动模式采样物理线程峰值 6、空闲 3，空闲回落并完成已提交退役记录回收的中位数约 48–49ms。RSS 指标见相邻 JSON，仅为采样，不承诺分配器或栈池立即归还 OS。
- 自动模式短突发包含扩容启动成本，慢于固定 4；纯调度微任务固定 1 更快。自动策略不保证所有负载都优于固定配置。

## 审查与未覆盖

上轮完整 16 文件候选 `1f0ea36d…` 独立审查 PASS，无 P0/P1；两项 P2 是持续 OOM 下 drain 重试、ProfileInfo 分配失败的非阻塞诊断/恢复建议，未擅自新增 abort 或撤销语义。随后短任务分摊修复共四文件，未参与实现者的独立增量复审同样 PASS（P0/P1/P2 均 0），父会话已回读报告并确认产品候选指纹未漂移。两份审查均只读核代码与已有功能证据，不将其称为性能实跑；性能结论来自父会话真实矩阵。

收尾核对发现增量审查执行者越过只读边界运行了 `git checkout 1e9f0e8`，将 HEAD 留在同一基线的 detached 状态；产品文件逐一指纹和索引均未改变。父会话已保留全部改动并挂回原 `feat/processer-dynamic-scaling` 分支。该越界动作不算只读合规证据，代码结论与运行验证仍分别按上述证据判断。

未执行 TSan/ASan、ARM 弱内存、本地全量 CTest、生产长 soak、真实网络消费者联调、远端 CI。使用约定可替代的排空时间，未测 p95/p99。WSL 宿主与亲和 CPU 不独占，结果是本轮有限样本。顶层 AGENTS 的本地定向验证要求与流程文档本地全量要求有冲突，本轮按已接受计划执行冒烟、本次功能和直接耦合验证，不宣称满足两者。

## 可重复入口及证据

- 新回归/故障入口：`Test_scheduler_scaling`、`Test_scheduler_scaling_failures`；CTest 名称前缀 `Test_scheduler_scaling_`，每模式独立进程。
- benchmark target：`benchmark_processer_scaling`，支持 `--mode fixed-old|fixed-candidate|dynamic`、`--workers`、`--tasks`、`--runs`、`--warmup`、`--task-us`、`--output`。真实旧基线需以 `BBT_SCALING_LEGACY_RUNTIME` 编译同一源，不能用候选二进制自称 fixed-old。
- 原始记录：`tests/reports/work/perf/20261019-12d25e5-scaling/{validation,performance}/`（忽略，不入仓）。其中 `checks.json`、`additional-checks.json`、`red.json`、`green-probe.json`、`matrix-commands.json`、`matrix-raw.json`、`environment.json`、`build-commands.json` 与日志可核对计数及退出码。
- 本目录只归档结论与关键指标；不提交原始日志、构建树、二进制或完整采样序列。
