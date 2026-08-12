# CI 稳定性故障调查与修复（2026-08-13）

> 范围：bbtools-coroutine PR #218（fix/scheduler-cold-restart）在 self-hosted runner `ubuntu-persion` 上的确定性 CI 失败调查。
> 结论性质：可复用故障结论（后续排查 CI 慢环境失败与 sanitizer 假阳性时引用）。

## 结论摘要

| # | 问题 | 根因 | 修复 |
|---|------|------|------|
| 1 | `Test_reliability` 在 CI 确定性失败（398/400、60s timeout） | ① `t_DL_01` 的 10s deadline 在 CI 慢环境（吞吐约为本地 1/5）耗尽；② 多处无超时 `CountDownLatch::Wait()` 可永久挂起 | deadline 10s→30s；无超时 wait 全部加超时防护 |
| 2 | 超时防护首次上线后 CI 上 `WaitTimeout` 恒立即返回 -1 | **bbtools-core `CountDownLatch::WaitTimeout` 实现缺陷**：end_tm 未加 timeout，恒立即 ETIMEDOUT | 测试侧改自包含 deadline 轮询（对修复前后 core 均健壮）；core 侧修复见 biangbiangtools PR #5 |
| 3 | CI attempt 2 观察到 `ch.Write(999)` 在 Close 后返回 0 | Chan `Close()` 的 CHAN_CLOSE 置位在锁外，`Write` 首查 `IsClosed()` 后、入队前可被并发 Close 穿透 | Write 系列锁内复查；Close 置位移入锁内；`m_run_status` volatile → atomic |

## 关键发现：CountDownLatch::WaitTimeout 缺陷

```cpp
// 修复前（bbtools-core）：
end_tm = now;                       // ← timeout 参数未加入！
pthread_cond_timedwait(..., &end_tm) // count>0 时恒立即 ETIMEDOUT
```

影响：所有带超时的 latch 等待语义失效，等价于立即超时。**本地与 CI 表现差异的解释**：本地协程调度快，`WaitTimeout` 被调用时 count 已归零（返回 0）；CI 慢环境下 count 尚 >0，立即返回 -1。

排查要点：**同一测试本地过、CI 必失败且失败点是超时等待时，先怀疑所依赖库的超时语义实现**，而非调度本身。

## Chan Close 竞态

`Close()` 原实现先锁外置 `CHAN_CLOSE` 再拿锁；`Write()` 在首查 `IsClosed()` 与 `m_item_queue.push()` 之间无复查。修复后以 `m_item_queue_mutex` 串行化「关闭置位」与「入队」：谁先拿锁谁先线性化，Close 后 Write 恒 -1。

## 验证工具边界（重要）

协程库（boost.context 自定义栈、未做 `__sanitizer_start_switch_fiber` 标注）在动态分析工具下存在系统性假阳性：

| 工具 | 可用项 | 不可用项 |
|------|--------|----------|
| ASAN+UBSAN | **运行态**（unified_stress 疲劳，无错误） | 单测二进制：stack-use-after-return / unknown-crash（0x52a fake stack 区域）、stack-overflow 均为协程栈切换假阳性；UBSAN vptr 与 `-fno-rtti` 冲突必误报 |
| valgrind memcheck | **HEAP SUMMARY（泄漏权威）**：40917 allocs 全释放、0 bytes | 错误计数（506788）全是 fcontext 寄存器/TLS 假阳性，不可作为错误依据 |

已固化的规避（`scripts/build_sanitizer.sh`）：
- `-fno-sanitize=vptr`（项目 `-fno-rtti`）
- `ASAN_OPTIONS=detect_stack_use_after_return=0`（fake stack 无法跟踪协程栈）

**有效验证组合**：normal 构建 ctest 全绿 + valgrind HEAP SUMMARY 零泄漏 + ASAN unified_stress 运行态零错误。

## 已知待办

- [ ] 协程切换点增加 ASAN fiber 切换标注（`__sanitizer_start_switch_fiber` / `__sanitizer_finish_switch_fiber`），使单测在 ASAN 下可跑。当前为独立需求，未排期。
- [ ] runner `ubuntu-persion` 网络偶发 github.com:443 连接失败（checkout 步骤 exit 128），属基础设施问题，与代码无关；连续出现时需检查 runner 侧网络/代理。
- [ ] 待 core 修复（PR #5）发布后，coroutine 测试的 `WaitLatchWithDeadline` helper 可回退为直接 `WaitTimeout`（helper 对两个版本均健壮，非阻塞项）。
