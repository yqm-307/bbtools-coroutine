# M1-06d 文档、示例与迁移说明校准：实现与契约差异

契约真源：`agent-docs/2026-09-07-core-runtime-contract.md`
Issue：#282（依赖 #266/#280/#281，对应 PR #290/#303/#304）

## 已锁住（README 与实现一致性）

- 新增「三之二、停机与生命周期契约（v1 M1）」迁移表：Stop 取消式停机、
  Stop 后注册明确失败（noexcept=succ=false / 非 noexcept=抛
  runtime_error，与 #280 实现对齐）、CoPool::Release 取消式排空 +
  broken_promise（#281）、Hook blocking-FD/flags/timeout 契约（#260/#261）、
  等待中 close 唤醒 EBADF（#262）、detached 异常日志+计数（#267/#275）、
  Release 栈真 free（#279）、bbtco_desc 落库（#276）、worker 停顿告警
  （#277）、栈保护页 SIGSEGV（#278）、协作式取消（#266）。
- CoPool 示例：删除"等待所有任务完成/All tasks completed"的过时语义，
  改为运行中退出 + 未执行取消式排空（与 #281 行为一致）。
- API 表 `CoPool.Release` 行同步取消式描述。

## 校准原则

- README 每条新契约都对应一个已合并 PR 的实现 + 测试，不写未实现承诺。
- 旧「注意事项」中的正确建议（避免栈引用、defer 清理等）保留不动。

## 验证

- 纯文档变更；迁移表逐条与 Scheduler.cc #280 实现、CoPool.cc #281 实现、
  Hook.cc #260/#261/#262 行为核对过源码（本分支链上可编译验证）。
