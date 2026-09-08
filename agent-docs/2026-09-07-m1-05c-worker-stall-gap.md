# M1-05c Worker 无进展与调度停顿检测：实现与契约差异

契约真源：`agent-docs/2026-09-07-core-runtime-contract.md` §5
Issue：#277（PR 堆叠于 #276，依赖其 desc 落库）

## 已锁住

- **检测器**：调度线程（独立于 worker）在 `_FixTimingScan` 每拍扫描各 worker
  的执行快照。协程单次执行占用 worker 超过 `m_cfg_worker_stall_warn_ms`
  （默认 0=关闭，零开销路径）即产生一次 `WorkerStallInfo` 告警。
- **现场内容**：worker id、co id、desc（bbtco_desc 落库直读，见 #276）、
  本次已占用时长、该 worker 本地队列积压 backlog（区分"占死+饥饿"）。
- **去重**：同一停顿（同一 begin_ts）只报一次；spin 持续 1s 仍 1 条告警。
- **回调**：`m_ext_worker_stall_callback`（调度线程调用，契约不得阻塞/抛出；
  抛出被 try/catch 吞掉保调度线程）；未设置回调默认 stderr 一行。
- **快照协议**：worker 单写者 + 调度线程读者，seqlock（奇=写中、偶=稳定，
  读者校验 seq 前后一致）；worker 空闲/协程已让出时 `executing=false`，
  parked 协程（sleep/事件等待）不算停顿——无误报路径。

## 边界与限制

- 检测覆盖"用户代码占死 worker"（死循环、不经 Hook 的阻塞 syscall——与
  #260 blocking-FD 契约互补）。**不覆盖**：调度线程自身停摆（检测器即调度
  线程）；worker 间窃取导致的饥饿归 backlog 字段观测。
- 阈值扫描频率 = `m_cfg_scan_interval_ms`（默认 1ms），告警延迟 ≤1 拍。
- desc 快照定长 56B 截断（诊断字段，非真源）。
- 外部阻塞 FD 导致的停顿在 Resume 窗口内同样命中（executing=true 超阈值），
  无需单独路径。

## 验证

- 新增 `Test_worker_stall` 3/3：spin 检出+现场字段断言；1s 持续停顿恰好 1
  次告警；parked（sleep 300ms > 阈值 200ms）零误报。
- 测试用 RAII fixture 保证异常路径下释放 spin + Stop，`RUN_SERIAL` 防并发
  饥饿（spin 吃满核会让同机时序测试超时——实测出现过，加门后 23/23 稳定）。
- 全量 `ctest -j2` 23/23（全量重编；混链旧二进制曾致堆损坏假失败，属构建
  纪律非代码问题）。
