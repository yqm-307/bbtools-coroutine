# CFS与R4-noformat CPU短采样

结论：值得先调查本地队列重复锁与任务创建中的callable复制/分配；未证明红黑树排序是主要瓶颈。不改运行时，不承诺收益。

四格双worker用户CPU采样，各3秒，cpu-clock:u/499Hz/DWARF8192。每格另有1秒无采样对照。初始化包含，无预热。gprofng失败记录保留，后由私有perf取得有效样本。

| 场景 | 版本 | 样本数 | 调用链不可用 | self符号未解析 |
|---|---|---:|---:|---:|
| cpu-short-v1 | base | 2343 | 50.11% | 6.44% |
| cpu-short-v1 | candidate | 2044 | 55.04% | 9.88% |
| yield-steady-v1 | base | 2674 | 49.29% | 3.25% |
| yield-steady-v1 | candidate | 2996 | 50.03% | 2.14% |

全部样本具有实际self IP；缺链样本不推断父函数，火焰图单列灰色分支。self分类互斥且和为总样本数；内联逻辑并入父符号。lost samples均0。

| 场景 | 版本 | 锁self | callable/引用计数/分配self | 时钟self | global self |
|---|---|---:|---:|---:|---:|
| cpu-short-v1 | base | 10.80% | 9.13% | 4.61% | 2.30% |
| cpu-short-v1 | candidate | 12.92% | 9.10% | 5.68% | 0.39% |
| yield-steady-v1 | base | 18.74% | 3.96% | 10.40% | 17.13% |
| yield-steady-v1 | candidate | 11.42% | 2.07% | 21.43% | 0.07% |

建议按以下顺序取舍（尚未实现或验证收益）：

1. Investigate coalescing redundant owner-local queue operations/locks without holding locks across Resume or CommitPark。证据：Pick/Account/Enqueue each take the queue mutex; CFS Processer::_Run lock callers visible; tree nodes intrusive so not newly allocated。限制：Processer::_Run contains inline CFS logic; samples do not isolate each operation or prove contention; preserve admission/retire and publication ownership。
2. Reduce callable copies/allocations in task creation before changing scheduler algorithm。证据：Both short arms callable/refcount/allocation self about9%; Coroutine.cc:37 captures const callback by value; Context.cc:94 copies into m_user_main。限制：shared cost, not demonstrated CFS-specific regression; count/move correctness and final ownership need separate experiment。
3. Check reuse of already-read timing data, keep per-coroutine service accounting。证据：CFS yield clock self21.43%; Processer.cc:447,481 already reads monotonic around Resume; driver also clocks each op。限制：No callchain for many clock samples. Higher fraction does not imply higher absolute CPU/op. Do not remove measurement correctness or fairness billing。

只观测用户on-CPU；不含内核/off-CPU等待。百分比不能当可回收CPU或延迟。每格单次短采样，工具对照时长不同，不认证开销、显著性或退化来源。CFS短任务增量原因仍未完全解释。

报告HTML与四个交互SVG放于WebUI reports目录。原始数据留忽略build-411-cfs-profile-r1，不提交。产品源码与库指纹保持冻结身份；无提交/推送。

> 归档说明：本 PR 只收入该历史运行的结论级 Markdown 摘要；原文提及的 raw、JSON、构建树与诊断程序未随本 PR 交付。保留历史时点的状态，不以此声明当前版本已复测。
