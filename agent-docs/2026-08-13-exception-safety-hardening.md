# 异常安全加固与覆盖率补强（2026-08-13）

> 范围：bbtools-coroutine #166/#169/#170/#189-#194 系列问题的实现与验证结论。
> 性质：长期资产（后续异常路径排查与测试扩展的基线说明）。

## 修复清单

| PR | Issue | 内容 | 关键结论 |
|---|---|---|---|
| #219 | #189 | CoPool 协程异常隔离 | worker catch 三策略（忽略计数/回调/future），异常不再逃逸终结 worker（修复前 Release 永久挂起） |
| #220 | #190 | Hook errno 恢复 | 挂起型 hook 的协程层失败/异常路径恢复系统调用 errno；新增 recvfrom/sendto hook |
| #221 | #191 | connect/accept 异常安全 | EISCONN 视为成功；异常路径读 SO_ERROR 重置 socket 状态可重试 |
| #222 | #192 | **协程异常 terminate 修复** | 见下 |
| #223 | #193 | Defer unwind 覆盖 | RAII defer 在异常栈展开/嵌套协程/CoPool 任务路径均执行，LIFO 顺序验证 |
| #224 | #194 | Profiler 除零防护 + 覆盖率 | ProfileInfo elapsed=0 除零 UB；新增 Reset() 与 5 用例 |
| #225 | - | 压测脚本修复 | 见下 |

## 关键发现：协程异常默认行为（terminate）

`Context::_CoroutineMain` 的 catch 块在 `m_ext_coevent_exception_callback` 为空时 `throw;`，异常逃出 fcontext 入口（无 C++ 异常处理框架）触发 `std::terminate`。**任何未设置异常回调的应用，协程内抛异常即进程死亡**。

修复（#222）：无回调时吞掉并计数（`GlobalConfig::m_unhandled_exception_count`），协程状态置 CO_FINAL 由 processer 正常回收。回调存在时行为不变。

排查要点：**协程库的异常处理必须三选一（交付回调/交付 future/吞掉计数），绝不能 re-throw 出 fcontext 入口**。

## 压测脚本两处缺陷（#225）

1. **汇总误报 ZERO**：run_parallel_stress.sh 取日志最后一行，但每条进程日志每周期打印全部模块指标行，最后一行常为其他模块 0 值行。实测 6 模块全部正常（comutex 519 万 ops/10min），汇总却显示 5 个 ZERO。修复：按 name 筛选取本模块最后一条。
2. **job 级 hang 无兜底**：wait 无超时，单模块 shutdown 卡死会永久拖死 job（CI 曾 91 分钟 hang，取消释放 runner）。修复：单进程 timeout(DUR+120) 兜底。

## 待办

- [ ] #167：CoMutex/CoCond 深度异常安全（Lock 路径 assert 改错误码语义）——高风险重构，需设计评审
- [ ] 压测 hang 根因未定位：本地 10min 干净（1400 万 ops 零错误），CI 91min hang 无法复现，怀疑慢环境 shutdown 交互；脚本兜底后不再阻塞 CI，需长时复现或 instrument
- [ ] runner `ubuntu-persion` github.com:443 间歇断网（周期约 20 分钟断几分钟），全部 CI 失败均为 checkout、零测试失败
