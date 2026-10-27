# owner-local 第一版原型：未达到验收要求

结论：BLOCKED / 不采纳。实现预算耗尽，未启动性能采样，不追加实现轮次。本结果否定当前实现的完整性与正确性，不能据此否定 owner 本地轮转方向的性能价值。

## 范围与版本

- 基线：R4-noformat，source_id `7d9c295890105a978b6aee4e4109db7967acc8af218474747f21a1873f353c5f`。父会话 verify_snapshot 已通过。
- 候选：`build-411-owner-local-r1/implementation/source/`，无候选提交 SHA。
- 需求及停止线：`build-411-owner-local-r1/brief.md`，实现与聚焦验证最多 1200 秒。
- 父会话实际差异：`build-411-owner-local-r1/actual-candidate.diff`，SHA256 `a03562801baf8ea8f2ba2e6621922e231bc8baceacc7b389b7b69c4e444b60a8`。独立审查回执核对一致。该差异同时记录候选复制时缺失的隐藏文件；审查聚焦五个产品/测试变更。
- 实现委派：`deleg_3371da9a`；独立只读审查：`deleg_e85c8eb1`。后者结论 no_ship/high。审查不构建、不测试、不修改候选。

## 确认的阻塞

下列均为父会话读码与独立审查核对的静态可达路径结论，未执行动态复现或 TSan。

| 问题 | 证据与后果 | 最小修复方向 |
|---|---|---|
| 外部任务可能饥饿 | 候选 `Processer.cc:486-504` 始终优先私有队列；持续 yield 的 NORMAL 协程重新进入该队列，收件队列中的同优先级任务没有执行机会。周期读取 global 只是搬入收件队列。 | 在有界调度机会内实际接收并执行外部任务，加入受控持续 yield 的判别测试。 |
| 新增数据竞争 | `Processer.cc:201-205` 无锁读取私有 deque 的 size；owner 并发 push/pop；`Scheduler.cc:1059` 自动伸缩采样、`Profiler.cc:150,175` 查询会跨线程读取。调用者锁未保护 owner 容器操作。 | owner 发布 atomic 近似计数，外线程不读取私有容器内部状态。 |
| 按需分摊不可达 | `Processer.cc:1107-1125` 读取并清除 m_share_requested；全 bbt 符号检索没有任何置 true 路径。设计文档所述 Scheduler 请求链路未实现。 | 接通空闲 worker 请求与 owner 响应，验证真实多 worker 参与。 |

三项均违反 brief 明确验收要求。已读 READY 路径保留 CommitYield、当前指针清空与 FINAL 销毁；这些局部事实不代表整体竞争安全。退役代码加入私有队列排空，但未取得行为验证。

## 实际验证与证据边界

父会话读回实现者实时工具轨迹，确认直接运行 `Test_local_rotation` 输出 Running 4 test cases / No errors detected，`Test_smoke` 输出 Running 14 test cases / No errors detected。这些测试通过仅证明现有断言通过。

- 新增 FIFO/外部插入测试未固定单 worker，未保证插入时本地 actor 仍在持续 yield，仅检查有限任务最终计数。
- SharePrivateToGlobal 的任务没有 yield，不进入新增私有 READY 队列，也未记录多个 P 参与。
- RetireDrainPrivateQueue 没有触发扩缩容或退役。
- owner 长时间不 yield、释放阻塞门前其他 P 的进展未测试。
- Test_scheduler_exec、Test_ready_wakeup 与直接耦合 driver/resize 回归没有完成证据。
- CTest 曾在 source 根目录执行，输出 No tests were found；不能算 CTest 通过。之后直接二进制执行的成功输出有效，但不补足上述缺口。
- CMake 首次编辑失败后已成功补入 Test_local_rotation 注册；最终实际文件已核对，失败提示不代表注册仍缺失。
- 构建过程包含一次 configure 失败、一次全量构建工具超时，随后目标构建成功。实现者 verification.json 未完整呈现这些失败；不认可其“全部完成、无新增警告”的综合声明。
- 未独立验证程序/库与最终候选源码指纹一致性。

## 成本与性能

实现委派总耗时 1289.37 秒，超过 1200 秒预算；独立只读审查耗时 152.03 秒，在其 180 秒预算内。耗时来自宿主委派回执，包含工具及推理时间，不等同于编译 CPU 时间。

性能样本数为 0。ops/s、CPU μs/op、P50、P99、idle CPU 均未取得本候选数据；不能推断收益或退化幅度。正确性和需求验收未通过，未消耗后续三臂性能预算。

## 状态与后续边界

- 当前候选仅作为失败原型证据保留，不采纳为新基线，不提交、不推送、不合入。
- 本轮父会话未修改产品源码；主仓存在此前其他未提交内容，不声称工作区整洁，也不对跨会话变化作归因。
- 实现验收仍未完成；独立只读审查完成且拒绝候选；三臂性能比较受阻。
- 若后续继续，先修复以上三个根因，补真实公平性、分摊、退役和不 yield 判别用例，再跑原定聚焦回归。只有通过后才进入性能比较；本报告不授权追加预算。

该文件属于支撑本轮验收结论的阶段性证据，仅归档摘要；原始委派日志、构建输出和实验源码不提交。

> 归档说明：本 PR 只收入该历史运行的结论级 Markdown 摘要；原文提及的 raw、JSON、构建树与诊断程序未随本 PR 交付。保留历史时点的状态，不以此声明当前版本已复测。
