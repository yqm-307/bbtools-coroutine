# M1-01b 执行模型：实现与契约差异

契约真源：`agent-docs/2026-09-07-core-runtime-contract.md`
Issue：#265

## 已锁住

- 两个 Processer 可同时各跑一个协程（`Test_scheduler_exec`）
- 同一协程 `Resume` 不重叠（yield 循环 overlap_max==1）
- 协程内 TLS 有效；测试线程 `GetLocalCoroutineId()==0`
- 挂起后 Resume 时 TLS 仍指向某个合法 Processer。不要求每次换 worker

## 缺口（不在本 Issue 改）

- 无公开 Processer 绑定/查询 API
- 协程内阻塞 OS 等待会占死该 worker（本测试用 CountDownLatch 探针，不引入看门狗）
- 取消与 Stop 留给 #266/#267/#280
