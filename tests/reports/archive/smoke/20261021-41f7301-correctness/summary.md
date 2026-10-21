# Runtime correctness 交付验收记录（提取候选，本地验证通过）

- 基线：`41f73016a248ae40b2eeca0131a8688fc68b3b40`；候选分支：`fix/runtime-correctness-batch-367-404`，未提交工作树。
- 本轮交付候选 18 个源码、测试与 API 文档文件的 manifest SHA256：`c5d40f25c593abbbe643d18c43ddef018e3e03c8b42d7b6c0b850b4efca46c90`，逐文件见 summary.json；不含两份归档，不是 commit SHA。
- 范围：#368、#369、#404 修复加 F-02 事件回调 registry 寿命修复。已排除 PR #406（#367）与 PR #407（#403）的实现改动：四个 stall 相关文件与 BASE 逐字节一致，Event 仅保留 F-02 registry holder 变化，构造回滚测试与其注册块不在本候选内。
- 状态：**本地验证通过；独立复审、新 PR 与 CI 仍待做，本次不构成 ship 或完成结论。**

## 提取候选实测矩阵（四档）

| 配置 | CTest | 注册 | 编译告警 |
|---|---|---|---|
| 默认（integrated） | 30/30 | 30 | 0 |
| STRINGENT_DEBUG（strict） | 30/30 | 30 | 0 |
| PROFILE | 30/30 | 30 | 0 |
| STRINGENT_DEBUG + DEBUG_INFO | 30/30 | 30 | 0 |

四档均按串行 CTest 执行，测试显式绑定本档 `LD_LIBRARY_PATH` 并逐目标 ldd 确认实际链接本档库（每档 26 条匹配库 ldd 记录）。生产配置（`-DNDEBUG`，无 `BBT_COROUTINE_STACK_TESTING`）增量构建通过，37 条库编译命令全部含 `-DNDEBUG`、0 条含 stack testing 宏，无新增告警。

strict 与 debug-info 档专用诊断测试每次实际运行 7 个 case，stderr 命中 5 次事件登记分配失败与 1 次 resume-co 分配失败，0 次 yield-co 缺口报告。原 first-winner 公共 API 探针在两档 × notify/cancel × fault 0/1 共 8/8 通过；原 DEBUG_INFO 探针在当前库上仍为 `caught=1 hits=1 callback_calls=0 status_running=1 await_reusable=1`。PROFILE 档 `Test_cowaiter_outcome` 以 `--repeat until-fail:100` 复跑：100 次开始、100 次通过、0 次失败；CTest 汇总按唯一测试计数显示 `out of 1`，因此不以汇总数字 100 为依据。

## F-02：事件回调 registry 寿命

有界取证取得一次真实故障：第 542 轮 SIGSEGV，前 541 轮正常，elapsed 146.26 秒，首个失败即停并保留信号与 core，取证前后二进制与库指纹不变。core 全线程快照显示主线程在 `__cxa_finalize` 析构 Event 全局 callback map 时释放已被回收的闭包与节点，坏 weak 控制块指针恰与 glibc `tcache_key` 重合；worker 与 Scheduler 线程当时仍存活。修复把 map 与 mutex 经 `Event::_Registry()` 收进进程寿命持有者，与核心契约 §6「运行时按进程寿命、不在静态退出期析构」一致；正常取消与析构仍逐项删除回调。新增 `Test_event_callback_registry_lifetime` 三模式：`exit_live_event` / `normal_reclaim` / `cancel_reclaim`。

归因需分开陈述：该寿命机制在 BASE 已存在，但 BASE 的同命令复跑尚未捕获原始 SIGSEGV；不能据此断言它无法复现。已捕获故障的静态析构竞争有直接证据，而本批修改是引入崩溃还是改变时序后暴露既有缺陷，仍无法定性（origin=unknown）。

## 已知限制

- 旧 strict ordinal 4 探针依赖「先抛异常再释放 holder」的握手，恢复登记后互等，历史记录为退出码 1，不报通过；其覆盖的真实获锁、取消与后继进展等价回归已在 R3 后续测试中闭环。
- 组合候选（含 #367/#403 实现）的四档 33/33、0 告警与 300 秒 1118 次正常退出属独立证据，与本轮 30/30 + 100 次复跑的提取候选分开记录，两者不可互相替代。
- 本候选未运行远端 PR CI、本地全量 CTest、TSan、弱序平台或真实客户端长测。#384 更早的 FD 快照/Poller 间歇故障仍未解释；#402 仅静态调查，#400/#259 不纳入。持续 OOM 与任意用户 callback 异常自愈不作承诺。
- PR #406、#407 最近读取均为 OPEN、各 10 项 CI 通过但无用户 review，且均未合并；这两个 PR 的 CI 不代表本候选通过。

## 复用边界

原始日志、core、内存地址、私有调试器与工具脚本均在仓外，不入 Git。归档完成后 20 文件候选（18 源码 + 2 归档）已按实际文件冻结指纹，工作树无提交，故不记录 HEAD_SHA。若在另一环境续接，先固定源码、编译配置与库指纹再复用已有首故障即停采集入口。