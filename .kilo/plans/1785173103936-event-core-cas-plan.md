# CoPollEvent 事件核心 CAS 状态机实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans` 逐任务实现此计划。步骤使用复选框（`- [ ]`）语法来跟踪进度。

**目标：** 在不修改 Chan、CoWaiter 上层业务语义或 Scheduler 生命周期的前提下，用 CAS 状态机消除 CoPollEvent 的提前触发竞态，并让 Processer 在安全交接点后才允许同一 Coroutine 被再次 Resume。

**架构：** CoPollEvent 是一次性等待 ticket，统一接收 user、system 和 timeout 来源；`INITED/ARMING/ARMED` 阶段的完成只记录 pending，Processer 完成当前 Coroutine 的最后一次访问后通过 `CommitPark()` 将 ticket 发布为 `PARKED`。普通 `bbtco_yield` 不创建 CoPollEvent，但同样延迟到 Processer 收尾后重新入队。Coroutine 执行体继续由当前手动生命周期管理，Scheduler registry、Pause/Resume、Stop 取消和 Chan 重构不在本计划内。

**技术栈：** C++17、Boost.Context、libevent/`bbt::pollevent`、Boost.Test、CMake/CTest、现有 lock-free Coroutine queue。

---

## 文件边界

**将修改：**

- `bbt/coroutine/detail/Define.hpp`：增加事件阶段和完成结果的内部定义，保留现有事件类型与兼容返回码。
- `bbt/coroutine/detail/CoPollEvent.hpp`、`bbt/coroutine/detail/CoPollEvent.cc`：实现原子状态、pending 结果、arm/park/trigger/cancel 的唯一胜者规则，以及底层 Event 的安全所有权转移。
- `bbt/coroutine/detail/Coroutine.hpp`、`bbt/coroutine/detail/Coroutine.cc`：记录 yield 类型、处理 event arm 失败、提供 Processer 使用的 `CommitYield()` 入口。
- `bbt/coroutine/detail/Processer.cc`：把普通 yield 入队和 event park 提交移动到 `Resume()` 统计完成之后。
- `unit_test/CMakeLists.txt`：注册事件核心状态测试目标。

**将创建：**

- `unit_test/Test_copollevent_state.cc`：只测试 CoPollEvent 状态转换、首胜、pending、取消和多 Processer 交接，不承担 Chan 或锁的业务测试。

**本计划明确不修改：**

- `bbt/coroutine/sync/Chan.hpp`、`__TChan.hpp`、`CoWaiter`、`CoCond`、`CoMutex`、`CoRWMutex` 的业务队列和并发模型。
- `Scheduler` 的 Start/Stop、Pause/Resume、协程 registry、优雅取消和冷重启。
- public API、hook、CMake 安装导出和 README。

## 设计约束

1. Coroutine 执行体仍由裸指针在 global/local queue 与 Processer 之间流转；本计划不在调度热路径引入 `shared_ptr` 或额外系统 mutex。
2. 一个 CoPollEvent 只绑定当前实现支持的来源组合：一个 system handle/fd 事件、可选 timeout、一个 user/custom trigger；任一来源首胜。
3. pending 只属于已经创建的本次 event ticket，不是全局粘性通知。尚未创建等待 ticket 时发生的 Notify 仍不保存。
4. `Trigger()`、`UnRegist()` 和 park 提交在同一原子阶段字上竞争。终态不可回退，事件不可复用，callback 最多执行一次。
5. `Regist()` 的现有 `0/-1` 返回约定保留；pending 视为 arm 成功，实际 callback 由 `CommitPark()` 消费。
6. `UnRegist()` 仍表示静默取消，供当前异常/清理路径使用；本阶段不实现 Stop 注入的取消异常。

## 状态与交接协议

实现一个 `std::atomic<uint64_t>` 状态字，低位保存 phase，高位保存首个触发结果 flags。推荐阶段及转换如下：

```text
INITED -> ARMING -> ARMED -> PARKED -> TRIGGERING -> FINAL
   |        |         |
   +--------+---------+---- Trigger -> PENDING -> CommitPark -> TRIGGERING
INITED/ARMING/ARMED/PARKED --------------- Cancel -> CANCELLED
```

- `Regist()` 通过 CAS 获取 `INITED -> ARMING` 的 arm 权；它负责启动底层 system Event。若进入 `Regist()` 时已经是 `PENDING`，说明提前事件已经获胜，`Regist()` 清理尚未启动的 system Event 并直接返回成功。
- system callback 或 user `Trigger()` 在 `INITED/ARMING/ARMED` 阶段只 CAS 到 `PENDING` 并把 flags 一并写入同一个原子字，不调用 Coroutine callback。
- `Regist()` 完成底层注册后把 `ARMING -> ARMED`；如果期间已是 `PENDING`，取消/延迟销毁刚创建的底层 Event，返回成功而不重复注册。
- Processer 在 `Coroutine::Resume()` 返回并完成运行时间统计后调用 `Coroutine::CommitYield()`；event wait 分支再调用 `CoPollEvent::CommitPark()`。该调用链是本轮对 Coroutine 的最后一次访问：`ARMED -> PARKED` 表示继续等待，`PENDING -> TRIGGERING` 表示由当前 Processer 完成一次激活。
- `PARKED -> TRIGGERING` 的 Trigger 获胜者负责 detach/defer 底层 Event、执行 Coroutine callback，并最终进入 `FINAL`。
- Cancel 只允许从 `INITED/ARMING/ARMED/PARKED` 取得终态；`PENDING/TRIGGERING/FINAL` 已经有完成胜者，不能再被取消覆盖。取消路径不执行普通恢复 callback。
- `m_onevent_callback`、event id、custom key、fd、timeout 等初始化后不可变。Getters 不再从可能被并发转移的 `m_event` 读取可缓存元数据。
- 底层 `m_event` 的 shared ownership 通过阶段所有权转移保护：ARMING 期间只由 arm 线程处理，PARKED 后只由 Trigger/Cancel 胜者处理；禁止并发 move/reset。
- `PARKED` CAS 和普通 ready queue enqueue 都是旧 Processer 对 Coroutine 的发布操作。发布前完成全部统计和字段更新，发布后不得再次读写该 Coroutine。
- 完成胜者在执行 callback 前先进入不可重复触发的终态，并用 `shared_from_this()` 保持 CoPollEvent 存活；callback 必须是尾调用路径，返回后不得再次访问 Coroutine。

## 实现任务

### 任务 1：建立状态机回归测试

**文件：**

- 创建：`unit_test/Test_copollevent_state.cc`
- 修改：`unit_test/CMakeLists.txt`

- [ ] **步骤 1：先写直接状态测试**

测试使用 `CoPollEvent::Create()` 和原子 callback 计数，不依赖 Chan 或锁。覆盖以下断言：

```cpp
auto event = CoPollEvent::Create(1, [&](auto, int flags, int) {
    ++callback_count;
    callback_flags.store(flags);
});
BOOST_REQUIRE_EQUAL(event->InitCustomEvent(POLL_EVENT_CUSTOM_COND, nullptr), 0);
BOOST_REQUIRE_EQUAL(event->Trigger(POLL_EVENT_CUSTOM), 0);
BOOST_REQUIRE_EQUAL(event->Regist(), 0);
BOOST_CHECK_EQUAL(callback_count.load(), 0); // 尚未 CommitPark
BOOST_REQUIRE(event->CommitPark());
BOOST_CHECK_EQUAL(callback_count.load(), 1);
BOOST_CHECK_EQUAL(event->CommitPark(), false); // 不重复完成
```

另外添加：`Regist -> CommitPark -> Trigger`、重复 Trigger、`Trigger` 与 `UnRegist` 竞争、pending 结果 flags 保留、终态不可复用等用例。测试不能依赖固定 sleep；使用 `std::barrier` 等价的 C++17 CountDownLatch/原子门闩协调线程。

- [ ] **步骤 2：把测试目标注册到 CMake**

在 `unit_test/CMakeLists.txt` 增加：

```cmake
add_executable(Test_copollevent_state Test_copollevent_state.cc)
target_link_libraries(Test_copollevent_state ${MY_LIBS})
add_test(NAME Test_copollevent_state COMMAND Test_copollevent_state)
```

- [ ] **步骤 3：运行测试确认当前实现失败或无法编译**

运行：

```bash
cmake -S . -B build -DNEED_TEST=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target Test_copollevent_state -j$(nproc)
ctest --test-dir build -R '^Test_copollevent_state$' --output-on-failure
```

预期：新 API/状态行为尚未实现时出现编译失败或测试失败；该结果作为 TDD 基线，不调整断言来适配旧行为。

### 任务 2：实现 CoPollEvent 原子状态与 pending 完成

**文件：**

- 修改：`bbt/coroutine/detail/Define.hpp`
- 修改：`bbt/coroutine/detail/CoPollEvent.hpp`
- 修改：`bbt/coroutine/detail/CoPollEvent.cc`

- [ ] **步骤 1：定义 phase、结果打包和 CAS 辅助函数**

在 detail 层定义明确的 phase 常量和状态字辅助函数。phase 与 flags 必须在同一个 CAS word 中更新，禁止先写结果再 CAS phase，避免失败者覆盖胜者结果。所有状态转换使用 acquire/release 语义，竞争转换使用 acq_rel。

- [ ] **步骤 2：将现有初始化和查询迁移到原子状态**

`InitFdEvent`/`InitCustomEvent` 只允许在 `INITED` 前完成；初始化完成后不再修改 callback、custom key、fd、timeout 等元数据。`IsListening()` 对 `ARMING/ARMED/PARKED` 返回 true；`IsFinal()` 只对 `FINAL/CANCELLED` 的终态按现有调用方预期返回完成；若现有 `GetStatus()` 需要保留旧枚举，增加内部 phase 到旧状态的映射，不让调用方读取非原子字段。

- [ ] **步骤 3：实现 `Regist()` 的 arm 协议**

`Regist()` 先处理已经存在的 `PENDING`，否则取得 `INITED -> ARMING` 后启动 system Event；custom-only event 不需要底层注册。底层启动期间发生 Trigger 时只产生 `PENDING`。启动返回后：

```text
ARMING -> ARMED       // 尚未完成
PENDING -> success    // 由后续 CommitPark 消费
CANCELLED -> failure  // 清理底层 event
```

底层启动失败必须进入失败终态并释放其 Event；不得留下可再次 Trigger 的对象。

- [ ] **步骤 4：实现 `Trigger()`、`CommitPark()` 和 `UnRegist()`**

`Trigger()` 在未 PARKED 阶段只写 pending outcome；在 `PARKED` 阶段取得 `TRIGGERING` 后才 detach/defer Event。完成胜者必须先把状态收敛为终态并自持有 event，再把 callback 作为最后动作执行。`CommitPark()` 只允许 `ARMED -> PARKED` 或 `PENDING -> TRIGGERING`，并保证 pending callback 只执行一次。`UnRegist()` 只对 `INITED/ARMING/ARMED/PARKED` 取得 `CANCELLED` 并清理资源，不能把已经获胜的 `PENDING/TRIGGERING` 改写成取消。

- [ ] **步骤 5：运行状态测试**

运行：

```bash
cmake --build build --target Test_copollevent_state -j$(nproc)
ctest --test-dir build -R '^Test_copollevent_state$' --output-on-failure
```

预期：任务 1 的状态、pending、首胜和取消测试全部通过。

### 任务 3：接入 Coroutine 的 park 结果和 Processer 安全交接

**文件：**

- 修改：`bbt/coroutine/detail/Coroutine.hpp`
- 修改：`bbt/coroutine/detail/Coroutine.cc`
- 修改：`bbt/coroutine/detail/Processer.cc`

- [ ] **步骤 1：定义 Coroutine 的 yield disposition**

增加仅供 runtime 使用的 disposition：普通 ready yield、event wait、manual yield/final。`YieldAndPushGCoQueue()` 只设置 ready disposition 并切出，不再在 Context callback 中直接调用 `Scheduler::OnActiveCoroutine()`。event wait 保留现有 `m_await_event`，由 Processer 收尾阶段提交 park。

- [ ] **步骤 2：让 event callback 只负责 arm 和资源锁释放**

保留 `YieldWithCallback` 的 callback 机制，以兼容现有 CoWaiter/CoMutex 调用方式；callback 中的 `Regist()` 成功包括 pending 状态，但不入队。`Coroutine` 自己创建的 fd/timeout event 在 arm 失败时清空 `m_await_event` 并返回失败，不能把坏 event 带入下一次 yield。

- [ ] **步骤 3：增加 `Coroutine::CommitYield()`**

该方法根据 disposition：

```text
READY      -> 返回需要重新入队
EVENT_WAIT -> 调用 m_await_event->CommitPark()
FINAL      -> 返回可回收
```

如果 pending 在这里获胜，由 CoPollEvent 调用现有 `OnCoPollEvent()`，设置 resume event、清空 await event 并完成一次入队。`CommitYield()` 必须在发布 PARKED 或执行 ready requeue 前完成所有 Coroutine 字段更新；发布动作之后，Processer 不得再次读取这个 Coroutine 的状态或字段。

- [ ] **步骤 4：调整 Processer 收尾顺序**

在 `Processer::_Run()` 中保持现有 `Resume()`、运行时间统计顺序，然后只调用一次 `CommitYield()`：普通 yield 在这里入队，event wait 在这里发布 PARKED/pending 完成，final 在这里交给现有删除路径。ready enqueue 或 PARKED 发布必须是该分支对 Coroutine 的尾操作。删除 `YieldWithCallback`/`YieldAndPushGCoQueue` callback 中的直接 requeue，避免新 Processer 在旧 Processer 完成统计前取得同一 Coroutine。

- [ ] **步骤 5：运行核心与现有协程回归**

运行：

```bash
cmake --build build --target Test_copollevent_state Test_coroutine Test_g_co Test_coevent -j$(nproc)
ctest --test-dir build -R '^(Test_copollevent_state|Test_coroutine|Test_g_co|Test_coevent)$' --output-on-failure
```

预期：状态测试、手动 Context 测试、普通 yield 测试及现有 fd/timeout event 测试通过；失败时先定位是事件状态、Processer 交接还是旧测试生命周期依赖，不通过增加 sleep 掩盖。

### 任务 4：增加交错压力和异常路径验证

**文件：**

- 修改：`unit_test/Test_copollevent_state.cc`
- 修改：`unit_test/CMakeLists.txt`（如需为测试设置 CTest `TIMEOUT`）

- [ ] **步骤 1：添加 trigger/commit 竞态循环**

每轮创建独立 event，先完成 arm，再用两个线程同时执行 `Trigger()` 和 `CommitPark()`，重复足够多轮，断言 callback 次数始终为 1，且最终 phase 只为 `FINAL` 或 `CANCELLED`。另测多个 trigger 线程竞争，只有一个调用返回胜者。

- [ ] **步骤 2：添加 system event 回归交错**

使用 pipe/timer 创建 system event，在注册刚完成和 Processer commit 前后分别写入/触发，断言协程只恢复一次，fd Event 只被 defer/destroy 一次。继续覆盖 timeout 与 user trigger 的首胜竞争。

- [ ] **步骤 3：添加普通 yield 多 Processer 压力测试**

创建多个协程反复 `bbtco_yield`，在用户代码中用原子计数记录每次进入/退出，测试结束后验证迭代总数和每个协程的完成次数，确保同一 Coroutine 没有重复 Resume 或丢失 requeue。

- [ ] **步骤 4：运行带超时的回归**

运行：

```bash
ctest --test-dir build -R '^(Test_copollevent_state|Test_coroutine|Test_g_co|Test_coevent)$' --output-on-failure
```

为新测试设置有限 CTest `TIMEOUT`，避免状态机死锁把测试进程永久挂起。

### 任务 5：Sanitizer、完整回归与性能基线

**文件：**

- 不新增源文件；使用现有 CMake 构建目录和 benchmark target。

- [ ] **步骤 1：执行 ASAN/UBSAN 核心测试**

使用独立构建目录开启 `-fsanitize=address,undefined -fno-omit-frame-pointer`，构建并运行 `Test_copollevent_state`、`Test_coroutine`、`Test_coevent`。预期无 UAF、double free、栈破坏或未定义行为报告。

- [ ] **步骤 2：执行 TSAN 交错测试**

使用独立构建目录开启 `-fsanitize=thread`，运行状态竞态与普通 yield 多 Processer 测试。预期不出现新增的 CoPollEvent、Coroutine 状态、event ownership 或重复入队数据竞争。

- [ ] **步骤 3：执行完整 CTest 回归**

运行：

```bash
ctest --test-dir build --output-on-failure
```

若已有测试依赖 `Start/Stop` 执行顺序，记录其失败的具体生命周期原因，不在本计划中修改 Scheduler 生命周期。

- [ ] **步骤 4：比较事件和调度性能**

在修改前后分别运行 `benchmark_coroutine`、`fatigue_coroutine` 以及可用的 event/timeout 基准，记录上下文切换吞吐、事件完成吞吐和内存错误。验收约束是核心热路径没有新增 `shared_ptr<Coroutine>` 引用计数或系统 mutex；若基准显示 CAS 状态字成为瓶颈，记录数据作为后续独立优化依据，而不在本增量中改变正确性协议。

## 完成标准

- CoPollEvent 的 pending、首胜、取消和终态转换有确定性测试覆盖。
- Processer 在完成 `Resume()` 收尾前不会让其他 Processer 取得同一 Coroutine。
- 普通 yield 和现有 fd/timeout event 行为保持兼容。
- 新增核心测试、现有相关测试和完整 CTest 通过；ASAN/UBSAN/TSAN 无新增问题。
- 事件核心路径不引入 Coroutine shared ownership 或额外系统 mutex。
- Chan、同步原语业务语义、Scheduler 生命周期和公共 API 没有被本增量顺带修改。
