# M1-06b Scheduler Stop 停机契约：实现与契约差异

契约真源：`agent-docs/2026-09-07-core-runtime-contract.md` §6
Issue：#280

## 已锁住

- **停机排空 = 真回收**：全局队列与 Processer 本地队列的 drain 从"置空指针裸丢"
  改为逐个 `delete`（协程对象 + fcontext 栈归还栈池）。旧行为 Release 下每个
  未完成任务泄漏对象与栈。
- **parked 协程纳入停机回收**：`_RegistAwaitEvent` 成功后协程进入 parked 登记
  （唯一引用是事件回调裸 this，脱离一切队列）；唤醒时（`OnCoPollEvent`）注销，
  不变式：协程同一时刻只属于 parked 表或某个队列。`Stop` 在全部 worker、
  Scheduler 线程、DNS worker join 完成后调 `Coroutine::DestroyParkedCoroutines`：
  逐个 `UnRegist` 事件（置 CANCELLED、摘出 #262 fd 唤醒表）后销毁——取消式
  停机，parked 协程不复活执行，与"不保证业务任务完成"一致。
- **停机后注册明确失败**：`RegistCoroutineTask` 在 `!m_is_running` 时抛
  `std::runtime_error`；noexcept 版捕获后 `succ=false`。旧行为 Release 下只打
  stderr、`succ=true` 假成功且泄漏协程（Debug 下 assert abort）。
- **重复 Stop 安全立即返回**（基线已满足，测试锁定：第二次 Stop <100ms）。
- **停机窗口唤醒即回收**：`OnActiveCoroutine` 在 `m_is_running=false` 后就地
  delete（不再入队给无人消费的队列），配合 parked 表先注销后入队的顺序，
  Stop 期间被 `Hook_Close`/定时器唤醒的 parked 协程无悬垂、无双释放。
- **Stop 有界**：存在 parked 协程（无限 fd 等待 + 60s 定时器）时实测 50ms 返回
  （scan interval），不阻塞等待业务完成。

## 已知限制（竞态窗口）

- Stop 执行 parked 回收期间，**外部非调度线程**同时调 `Hook_Close` /
  `NotifyCustomEvent`，`Trigger` 持事件 `SPtr` 可能穿过 swap→UnRegist 间隙访问
  正在 `delete` 的协程（UAF 窗口，微秒级）。前提是把 Stop 当静止操作使用
  （契约本意：停机时刻不应有外部线程继续对运行时发起操作）。彻底消除需协程
  生命周期 shared_ptr 化入事件回调，超出本 Issue 范围。旧代码同场景是
  "永久 parked + 泄漏"，不比现在好。入队方向已由 OnActiveCoroutine 停机检查兜住。
- `Processer::Stop` 的 5s 超时强杀分支若触发（用户代码卡死 worker），
  drain delete 与残存 `_Run` 的 `m_running_coroutine` 无交集（已出队），
  无双重释放；该分支本身的前提（worker 被非 hook 阻塞代码卡死）仍由
  #277（无进展检测）承接。

## 验证

- 新增 `Test_scheduler_stop` 3/3：parked（fd 无限等待 + 60s 定时器）Stop 50ms
  有界返回；重复 Stop 第二次 <100ms；停机后 noexcept 注册 succ=false、
  抛出版可捕获。修复前 t1 挂死 >12s（RED）。
- 全量 `ctest -j2` 25/25 通过（含 #260/#261/#262 全部 Hook 回归）。
- 停机后重启（fixture 每用例 Stop→Start）路径反复验证无堆积。
