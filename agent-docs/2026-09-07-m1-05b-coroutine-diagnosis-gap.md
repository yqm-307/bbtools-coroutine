# M1-05b 协程诊断现场与描述信息：实现与契约差异

契约真源：`agent-docs/2026-09-07-core-runtime-contract.md` §5
Issue：#276

## 已锁住

- **desc 落库**：`bbtco_desc(desc)` 不再是丢弃参数的薄壳——`_CoHelper(const char*)`
  → `RegistCoroutineTask(handle, desc)` → `Coroutine::m_desc`。`GetDescription()`
  协程内读回；普通 `bbtco`/`bbtco_noexcept` 默认空描述，行为不变。
- **等待现场**：`GetWaitInfo(CoroutineWaitInfo&)` 返回 ID 之外的现场：状态由
  `GetStatus()` 给出；等待类型 `m_wait_event`（PollEventType 位口径）、等待对象
  `m_fd`（非 fd 等待为 -1）、定时器总时长 `m_timeout_ms`、已等待 `m_waited_us`
  （`_RegistAwaitEvent` 成功即记 parked 起点，`OnCoPollEvent` 唤醒清零）。
  未处于事件等待返回 -1。
- 每次唤醒事件仍保留 `GetLastResumeEvent()`（既有）。

## 边界与限制

- `GetWaitInfo` 契约上只允许**协程自身（同 Processer 线程）**读取：parked 成员
  无锁，依赖"协程同一时刻单线程访问"的调度不变式。跨线程统一诊断快照属
  #277（worker 无进展检测）范围，本 Issue 不扩。
- desc 是 `const char*` 字面量口径，注册时拷贝进 `std::string`，不悬垂。
- 默认参数保持 `RegistCoroutineTask(handle)` 源码兼容（ABI 变，项目未发布，
  全量重编即可——本轮已验证 22/22）。

## 验证

- 新增 `Test_coroutine_diag` 4/4：desc 落库读回、普通 bbtco 空描述、唤醒后
  现场清零 + LastResumeEvent 保留、未挂起时 GetWaitInfo=-1。
- 全量 `ctest -j2` 22/22（含 #270 宏回归，证明 bbtco_desc 语义升级不破坏
  既有 wrapper 契约）。
