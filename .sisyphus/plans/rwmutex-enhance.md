# CoRWMutex Enhancement Plan

## TL;DR

> **Quick Summary**: Repair CoRWMutex’s wakeup/state-transition bugs, promote its API from a generic `UnLock()` model to explicit read/write unlock paths, add timeout-capable try-lock APIs, and raise confidence with stronger unit coverage plus a dedicated fatigue test.
>
> **Deliverables**:
> - Corrected `CoRWMutex` state machine and wakeup ordering
> - New `RUnLock/WUnLock`, `TryRLock`, and `TryWLock` APIs
> - Expanded `Test_co_rwmutex`
> - New `fatigue_corwmutex` benchmark-style stress program
> - Updated rwmutex debug sample and build wiring
>
> **Estimated Effort**: Medium
> **Parallel Execution**: YES - 2 main waves + final verification
> **Critical Path**: API/header design → core rwmutex implementation → tests/fatigue verification

---

## Context

### Original Request
完善 rwmutex 的测试案例，并 review 代码。

### Interview Summary
**Key Discussions**:
- 写优先是既定策略，不能退化成公平锁或读优先。
- `UnLock()` 当前实现存在 bug，且本次不是只修单点，而是整体修正解锁与唤醒策略。
- 本次变更除了单元测试，还要产出疲劳测试程序。
- 需要为 rwmutex 增加 `TryRLock/TryWLock`，并支持超时版本。
- 新 API 采用显式 `RUnLock` / `WUnLock`，避免统一 `UnLock()` 掩盖锁语义。

**Research Findings**:
- `bbt/coroutine/sync/CoRWMutex.cc` 当前在 `UnLock()` 中先 `_NotifyOne()` 再更新状态，存在错误的唤醒顺序。
- 当前实现使用 `CoWaiter` 管理 reader / writer 等待队列，`CoWaiter` 已支持 `WaitWithTimeoutAndCallback`。
- `CoMutex` 已有 `TryLock()` 与 `TryLock(int ms)`，可作为 rwmutex 的返回值和接口风格参考。
- 现有 `Test_co_rwmutex.cc` 只覆盖基本场景，缺少写优先、timeout、多 writer、疲劳型覆盖。
- `benchmark_test/fatigue_comutex.cc` 已提供疲劳程序模式参考。

### Manual Gap Review
**Identified Gaps** (addressed in this plan):
- **旧 `UnLock()` 是否保留**：默认不保留，直接迁移仓库内 rwmutex 调用点，并在总结中披露为默认决策。
- **TryLock timeout 的等待节点残留风险**：计划中要求显式验证 timeout 后 waiter 不会破坏后续唤醒路径。
- **是否扩散到通用锁接口**：明确排除 `ICoLock` / `StdLockWapper` 级别的接口统一，避免把读写锁错误抽象成普通锁。

---

## Work Objectives

### Core Objective
把 CoRWMutex 从“基本可用但边界不稳”的状态提升到“写优先语义明确、解锁/唤醒正确、API 可表达读写差异、并具备压力验证”的状态。

### Concrete Deliverables
- `bbt/coroutine/sync/CoRWMutex.hpp`：新增/调整公开 API
- `bbt/coroutine/sync/CoRWMutex.cc`：修复状态迁移、等待与唤醒实现
- `unit_test/Test_co_rwmutex.cc`：新增高价值并发/timeout/写优先测试
- `benchmark_test/fatigue_corwmutex.cc`：新增疲劳测试程序
- `benchmark_test/CMakeLists.txt`：接入新 fatigue target
- `debug/Debug_co_rwmutex.cc`：同步新 API，保留调试入口可运行

### Definition of Done
- [ ] `CoRWMutex` 不再暴露统一 `UnLock()` 作为主要读写解锁接口
- [ ] `TryRLock()/TryRLock(int ms)/TryWLock()/TryWLock(int ms)` 可编译并通过测试
- [ ] `ctest -R Test_co_rwmutex --output-on-failure` 通过
- [ ] 新 fatigue 程序可编译，并能持续运行输出状态而不触发断言

### Must Have
- 保持写优先：一旦存在等待写者，新 reader 不得继续穿透获取锁
- 修复 notify 顺序：必须先更新状态再唤醒等待方
- 非最后一个 reader 解锁时不得无意义唤醒 writer
- writer 释放且无等待 writer 时应直接批量唤醒 readers
- TryLock timeout 返回语义清晰，并与 `CoMutex` 风格一致

### Must NOT Have (Guardrails)
- 不要把 rwmutex 改成公平锁或读优先锁
- 不要把本次任务扩展到 `ICoLock` / `StdLockWapper` 通用接口重构
- 不要顺手加入升级锁、降级锁、可重入等新语义
- 不要依赖“链式 reader 唤醒”掩盖 writer release 的唤醒缺陷
- 不要留下 timeout 后无法清理/跳过的僵尸 waiter 问题

---

## Verification Strategy

> **ZERO HUMAN INTERVENTION** - ALL verification is agent-executed.

### Test Decision
- **Infrastructure exists**: YES
- **Automated tests**: Tests-after
- **Framework**: Boost.Test + CTest
- **Agent QA**: Mandatory for every task

### QA Policy
- **Unit tests**: `Test_co_rwmutex` 覆盖 API、写优先、timeout、basic stress
- **Fatigue validation**: 通过独立 `fatigue_corwmutex` 程序长时间循环断言验证
- **Build wiring**: 验证 unit_test / benchmark_test target 可单独编译
- **Evidence directory**: `.sisyphus/evidence/`

---

## Execution Strategy

### Parallel Execution Waves

```text
Wave 1 (Foundation / core split)
├── Task 1: Public API reshape in CoRWMutex header [quick]
├── Task 2: Reader/writer wait-path + timeout helpers [unspecified-high]
├── Task 3: RUnLock/WUnLock state-transition rewrite [unspecified-high]
├── Task 4: Update debug rwmutex sample to new API [quick]
└── Task 5: Unit test skeleton migration to new API [quick]

Wave 2 (Behavior verification / stress)
├── Task 6: Add writer-priority + timeout unit cases [unspecified-high]
├── Task 7: Add fatigue_corwmutex program + CMake wiring [quick]
└── Task 8: Expand multi-reader/multi-writer stress assertions [unspecified-high]

Wave FINAL
├── Task F1: Plan compliance audit (oracle)
├── Task F2: Code quality review (unspecified-high)
├── Task F3: Real QA execution (unspecified-high)
└── Task F4: Scope fidelity check (deep)
```

### Dependency Matrix
- **1**: None → 2, 3, 4, 5
- **2**: 1 → 3, 6, 8
- **3**: 1, 2 → 4, 5, 6, 7, 8
- **4**: 1, 3 → F1-F4
- **5**: 1, 3 → 6, 8
- **6**: 2, 3, 5 → F1-F4
- **7**: 1, 3 → F1-F4
- **8**: 2, 3, 5 → F1-F4

### Agent Dispatch Summary
- **Wave 1**: T1 `quick`, T2-T3 `unspecified-high`, T4-T5 `quick`
- **Wave 2**: T6 `unspecified-high`, T7 `quick`, T8 `unspecified-high`
- **Final**: F1 `oracle`, F2-F3 `unspecified-high`, F4 `deep`

---

## TODOs

- [ ] 1. Reshape the public CoRWMutex API in the header

  **What to do**:
  - Update `bbt/coroutine/sync/CoRWMutex.hpp` to expose explicit read/write unlock paths: `RUnLock()` and `WUnLock()`.
  - Add try-lock API surface aligned with `CoMutex`: `TryRLock()`, `TryRLock(int ms)`, `TryWLock()`, `TryWLock(int ms)`.
  - Add any private helper declarations needed for timeout wait paths and wakeup dispatch.
  - Ensure the resulting API clearly communicates that read-lock and write-lock release are distinct operations.

  **Must NOT do**:
  - Do not alter `ICoLock`.
  - Do not add upgrade/downgrade/reentrant semantics.
  - Do not leave the class in a half-old/half-new API state without explicit migration intent.

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: focused header/API shaping with low algorithmic complexity.
  - **Skills**: `writing-plans`
    - `writing-plans`: keeps API/task scope crisp and avoids accidental architecture drift.
  - **Skills Evaluated but Omitted**:
    - `test-driven-development`: this task is API surfacing only; behavior validation lands in later tasks.

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 4, 5)
  - **Blocks**: 2, 3, 4, 5
  - **Blocked By**: None

  **References**:
  - `bbt/coroutine/sync/CoRWMutex.hpp:8-41` - current public/protected/private layout to preserve class shape while expanding API.
  - `bbt/coroutine/sync/CoMutex.hpp:35-58` - reference naming and overload style for `TryLock()` and `TryLock(int ms)`.
  - `bbt/coroutine/syntax/SyntaxMacro.hpp:60-64` - confirms external construction macro already exists and should remain valid.

  **Acceptance Criteria**:
  - [ ] `CoRWMutex.hpp` declares `RLock/WLock/RUnLock/WUnLock/TryRLock/TryWLock` with both timeout and non-timeout overloads.
  - [ ] No changes made to `ICoLock.hpp`.

  **QA Scenarios**:
  ```text
  Scenario: Header surface matches the new plan
    Tool: Bash (grep)
    Preconditions: Code changes applied
    Steps:
      1. Search `bbt/coroutine/sync/CoRWMutex.hpp` for `RUnLock`, `WUnLock`, `TryRLock`, and `TryWLock` declarations.
      2. Verify both no-arg and `int ms` overloads exist for try-lock methods.
      3. Verify `UnLock()` is not retained as the primary public rwmutex release API.
    Expected Result: All planned declarations exist and old unlock API is absent or explicitly retired.
    Failure Indicators: Missing overloads, lingering public `UnLock()` as main API, or accidental `ICoLock` edits.
    Evidence: .sisyphus/evidence/task-1-api-surface.txt

  Scenario: Generic lock abstraction remains untouched
    Tool: Bash (grep)
    Preconditions: Code changes applied
    Steps:
      1. Diff `bbt/coroutine/sync/interface/ICoLock.hpp` and `bbt/coroutine/sync/StdLockWapper.hpp`.
      2. Confirm no rwmutex-specific changes were introduced there.
    Expected Result: Both generic lock files remain unchanged.
    Failure Indicators: New rwmutex methods added to generic interface/wrapper.
    Evidence: .sisyphus/evidence/task-1-generic-lock-check.txt
  ```

- [ ] 2. Add explicit reader/writer wait helpers including timeout paths

  **What to do**:
  - Extend `bbt/coroutine/sync/CoRWMutex.cc` with private helpers for read/write waiting that support both indefinite wait and timeout wait.
  - Reuse `CoWaiter::WaitWithTimeoutAndCallback` safely for timeout-based try-lock flows.
  - Define return semantics consistent with `CoMutex`: `0` success, `1` timeout, `-1` failure.
  - Ensure timeout wakeups do not corrupt later queue traversal or make waiters permanently un-notifiable.

  **Must NOT do**:
  - Do not duplicate timeout semantics that diverge from `CoMutex`.
  - Do not ignore the stale-waiter risk after timeout.

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: queue correctness and timeout interaction are subtle concurrency concerns.
  - **Skills**: `systematic-debugging`
    - `systematic-debugging`: helps reason through wait-queue edge cases and timeout aftermath.
  - **Skills Evaluated but Omitted**:
    - `visual-engineering`: no UI concern.

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 1, 4, 5)
  - **Blocks**: 3, 6, 8
  - **Blocked By**: 1

  **References**:
  - `bbt/coroutine/sync/CoRWMutex.cc:123-145` - current `_WaitRLock/_WaitWLock` helper locations to expand rather than invent new structure elsewhere.
  - `bbt/coroutine/sync/CoWaiter.cc:114-153` - exact timeout and notify behavior for `WaitWithTimeoutAndCallback` and `Notify()`.
  - `bbt/coroutine/sync/CoMutex.cc:72-97` - expected timeout return flow and post-wakeup re-check pattern.

  **Acceptance Criteria**:
  - [ ] Read/write timeout waits use helper logic rather than duplicating queue code inline.
  - [ ] Timeout path re-checks lock state after wakeup and returns `1` for timeout.
  - [ ] Wait queue traversal remains valid even when earlier waiters time out.

  **QA Scenarios**:
  ```text
  Scenario: Timeout helper path compiles into rwmutex build
    Tool: Bash
    Preconditions: Code changes applied
    Steps:
      1. Build target `Test_co_rwmutex`.
      2. Confirm compiler accepts new helper declarations/definitions with no duplicate symbol or signature mismatch.
    Expected Result: Build succeeds.
    Failure Indicators: Undefined reference, duplicate definition, or signature mismatch errors.
    Evidence: .sisyphus/evidence/task-2-build.txt

  Scenario: Timed-out waiter does not poison later wakeups
    Tool: Bash (ctest)
    Preconditions: Timeout-specific unit test added in Task 6/8
    Steps:
      1. Run `ctest --test-dir build -R Test_co_rwmutex --output-on-failure`.
      2. Observe timeout case followed by successful subsequent acquisition in the same suite.
    Expected Result: Timeout case reports expected timeout and later lock acquisitions still pass.
    Failure Indicators: Subsequent acquisition hangs/fails after a timeout case.
    Evidence: .sisyphus/evidence/task-2-timeout-followup.txt
  ```

- [ ] 3. Rewrite reader/writer unlock and wakeup state transitions

  **What to do**:
  - Replace unified unlock behavior with separate `RUnLock()` and `WUnLock()` implementations in `CoRWMutex.cc`.
  - Enforce the new wakeup order: update internal state first, then wake the correct queue.
  - When a non-last reader unlocks, do not wake any writer.
  - When the last reader unlocks, prefer one writer if present; otherwise wake readers as appropriate.
  - When a writer unlocks, if writers are waiting then wake one writer; otherwise directly wake all readers.
  - Preserve write-priority via `m_has_wait_wlock` and ensure new readers are blocked while writers wait.

  **Must NOT do**:
  - Do not keep chain-based reader wakeup as the primary writer-release mechanism.
  - Do not weaken write-priority.

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: core correctness of the lock hinges on this logic.
  - **Skills**: `systematic-debugging`
    - `systematic-debugging`: appropriate for reasoning about race-sensitive state transitions.
  - **Skills Evaluated but Omitted**:
    - `quick`: too risky for a central synchronization rewrite.

  **Parallelization**:
  - **Can Run In Parallel**: NO
  - **Parallel Group**: Sequential core task
  - **Blocks**: 4, 5, 6, 7, 8
  - **Blocked By**: 1, 2

  **References**:
  - `bbt/coroutine/sync/CoRWMutex.cc:12-120` - existing `RLock/WLock/UnLock/_NotifyOne/_NotifyAll` flow that needs targeted replacement.
  - `bbt/coroutine/detail/Define.hpp:128-133` - existing rwmutex states to preserve rather than invent new enum states unnecessarily.
  - `unit_test/Test_co_rwmutex.cc:15-124` - current behavioral expectations that should still pass after API migration.

  **Acceptance Criteria**:
  - [ ] No path notifies waiters before state is updated.
  - [ ] Non-last reader unlock leaves waiting writer asleep.
  - [ ] Writer unlock with no waiting writer wakes all waiting readers directly.
  - [ ] Write-priority remains intact when readers and writers contend.

  **QA Scenarios**:
  ```text
  Scenario: Last reader hands off to waiting writer
    Tool: Bash (ctest)
    Preconditions: Unit case exists for reader-held lock with queued writer
    Steps:
      1. Run `ctest --test-dir build -R Test_co_rwmutex --output-on-failure`.
      2. Verify a case where one reader remains after another reader unlocks does not allow writer through.
      3. Verify the writer acquires only after the final reader unlock.
    Expected Result: Writer stays blocked until the last reader releases.
    Failure Indicators: Writer acquires early or deadlocks after final reader release.
    Evidence: .sisyphus/evidence/task-3-reader-to-writer.txt

  Scenario: Writer release directly wakes readers
    Tool: Bash (ctest)
    Preconditions: Unit case exists with one writer followed by multiple readers
    Steps:
      1. Run `ctest --test-dir build -R Test_co_rwmutex --output-on-failure`.
      2. Verify readers queued behind a writer all proceed after writer release when no writer remains queued.
    Expected Result: Multiple readers acquire after writer release without depending on chained single-reader wakeups.
    Failure Indicators: Only one reader proceeds or test hangs.
    Evidence: .sisyphus/evidence/task-3-writer-to-readers.txt
  ```

- [ ] 4. Update rwmutex debug sample to the new explicit unlock API

  **What to do**:
  - Update `debug/Debug_co_rwmutex.cc` so reader paths call `RUnLock()` and writer paths call `WUnLock()`.
  - Keep the sample behavior the same: many readers, a few writers, periodic output.
  - Ensure the sample still serves as a quick manual sanity harness for the new rwmutex behavior.

  **Must NOT do**:
  - Do not redesign the debug program into a benchmark framework.
  - Do not leave stale `UnLock()` calls in rwmutex-specific sample code.

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: small, isolated API migration.
  - **Skills**: `writing-plans`
    - `writing-plans`: keeps task narrowly scoped.
  - **Skills Evaluated but Omitted**:
    - `systematic-debugging`: core behavior already validated elsewhere.

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 1, 5)
  - **Blocks**: Final verification only
  - **Blocked By**: 1, 3

  **References**:
  - `debug/Debug_co_rwmutex.cc:15-53` - current reader/writer sample structure to preserve.
  - `debug/CMakeLists.txt:46-47` - existing target wiring proving this file should remain buildable.

  **Acceptance Criteria**:
  - [ ] `Debug_co_rwmutex.cc` compiles against the new rwmutex API.
  - [ ] No rwmutex `UnLock()` calls remain in the debug sample.

  **QA Scenarios**:
  ```text
  Scenario: Debug sample compiles with new API
    Tool: Bash
    Preconditions: Code changes applied
    Steps:
      1. Build target `Debug_co_rwmutex`.
      2. Confirm there are no compile errors related to missing `UnLock()` or wrong unlock method.
    Expected Result: Target builds successfully.
    Failure Indicators: Compile failure referencing stale unlock method names.
    Evidence: .sisyphus/evidence/task-4-debug-build.txt

  Scenario: Debug sample still expresses reader/writer paths correctly
    Tool: Bash (grep)
    Preconditions: Code changes applied
    Steps:
      1. Search the file for `RLock`, `WLock`, `RUnLock`, `WUnLock`.
      2. Confirm reader lambdas use `RUnLock` and writer lambdas use `WUnLock`.
    Expected Result: Unlock methods match their corresponding lock methods.
    Failure Indicators: Mixed read/write unlock usage.
    Evidence: .sisyphus/evidence/task-4-debug-api-check.txt
  ```

- [ ] 5. Migrate existing rwmutex unit test skeleton to the new API surface

  **What to do**:
  - Update `unit_test/Test_co_rwmutex.cc` existing test cases from `UnLock()` to `RUnLock()` / `WUnLock()`.
  - Preserve scheduler start/stop structure and current test intent.
  - Keep the file ready for additional cases added in later tasks.

  **Must NOT do**:
  - Do not silently drop existing cases.
  - Do not mix old unlock API with new API in the same file.

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: mechanical migration with low conceptual complexity.
  - **Skills**: `writing-plans`
    - `writing-plans`: keeps the test migration minimal and targeted.
  - **Skills Evaluated but Omitted**:
    - `test-driven-development`: deeper test expansion comes later.

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with Tasks 1, 4)
  - **Blocks**: 6, 8
  - **Blocked By**: 1, 3

  **References**:
  - `unit_test/Test_co_rwmutex.cc:15-124` - current test file needing API migration while preserving behavior intent.
  - `unit_test/CMakeLists.txt:53-55` - existing test target wiring.

  **Acceptance Criteria**:
  - [ ] Existing rwmutex tests compile against the new unlock API.
  - [ ] Scheduler bootstrap/shutdown cases remain intact.

  **QA Scenarios**:
  ```text
  Scenario: Existing rwmutex tests compile after API migration
    Tool: Bash
    Preconditions: Code changes applied
    Steps:
      1. Build target `Test_co_rwmutex`.
      2. Confirm stale `UnLock()` references do not break compilation.
    Expected Result: Build succeeds.
    Failure Indicators: Missing method compile errors or partial migration.
    Evidence: .sisyphus/evidence/task-5-test-build.txt

  Scenario: Legacy rwmutex unlock calls are fully removed from the test file
    Tool: Bash (grep)
    Preconditions: Code changes applied
    Steps:
      1. Search `unit_test/Test_co_rwmutex.cc` for `->UnLock(`.
      2. Verify zero matches.
    Expected Result: No stale generic unlock calls remain.
    Failure Indicators: Any `->UnLock(` match in the rwmutex test file.
    Evidence: .sisyphus/evidence/task-5-no-unlock.txt
  ```

- [ ] 6. Add writer-priority and timeout-focused rwmutex unit cases

  **What to do**:
  - Extend `unit_test/Test_co_rwmutex.cc` with focused cases for:
    - writer waits behind active readers and acquires only after last reader release,
    - new reader is blocked once a writer is queued,
    - `TryRLock(int ms)` times out under active writer,
    - `TryWLock(int ms)` times out under active reader(s),
    - timeout followed by later successful acquisition.
  - Use precise synchronization (`CountDownLatch`, `CoCond`, controlled sleeps) rather than only long free-running loops.

  **Must NOT do**:
  - Do not rely only on flaky sleep timing when stronger synchronization is available.
  - Do not test timeout in isolation without a post-timeout recovery case.

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: concurrency tests are easy to make flaky or logically incomplete.
  - **Skills**: `test-driven-development`, `systematic-debugging`
    - `test-driven-development`: focuses the task on observable lock semantics.
    - `systematic-debugging`: helps design tests around race-sensitive boundaries.
  - **Skills Evaluated but Omitted**:
    - `quick`: insufficient rigor for timing-sensitive concurrency coverage.

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2 (with Task 7)
  - **Blocks**: Final verification
  - **Blocked By**: 2, 3, 5

  **References**:
  - `unit_test/Test_co_rwmutex.cc:15-124` - existing structure for scheduler lifecycle and current rwmutex cases.
  - `unit_test/Test_comutex.cc:17-79` - reference style for time-window assertions and latch usage.
  - `bbt/coroutine/sync/CoMutex.cc:72-97` - timeout return expectations to mirror in rwmutex tests.
  - `bbt/coroutine/sync/CoWaiter.cc:114-153` - timeout and notify semantics influencing edge-case testing.

  **Acceptance Criteria**:
  - [ ] At least one test proves queued writer blocks later readers.
  - [ ] At least one test proves `TryRLock(int ms)` returns timeout under writer hold.
  - [ ] At least one test proves `TryWLock(int ms)` returns timeout under reader hold.
  - [ ] At least one test proves a later acquisition still succeeds after an earlier timeout.

  **QA Scenarios**:
  ```text
  Scenario: Writer priority is enforced when reader arrives late
    Tool: Bash (ctest)
    Preconditions: New priority test added
    Steps:
      1. Run `ctest --test-dir build -R Test_co_rwmutex --output-on-failure`.
      2. Inspect output for the case where Reader A holds, Writer W queues, then Reader B attempts entry.
      3. Confirm Reader B does not acquire before Writer W.
    Expected Result: Writer W acquires before Reader B.
    Failure Indicators: Reader B enters before Writer W or test hangs.
    Evidence: .sisyphus/evidence/task-6-writer-priority.txt

  Scenario: Timeout returns expected code and lock remains usable
    Tool: Bash (ctest)
    Preconditions: New timeout + recovery test added
    Steps:
      1. Run `ctest --test-dir build -R Test_co_rwmutex --output-on-failure`.
      2. Confirm timeout case returns `1` under contention.
      3. Confirm a follow-up acquisition later returns `0`.
    Expected Result: Timeout semantics match plan and lock recovers for subsequent use.
    Failure Indicators: Timeout returns wrong code, or follow-up acquisition fails/hangs.
    Evidence: .sisyphus/evidence/task-6-timeout-recovery.txt
  ```

- [ ] 7. Add a dedicated fatigue_corwmutex program and benchmark build wiring

  **What to do**:
  - Create `benchmark_test/fatigue_corwmutex.cc` following the style of existing fatigue programs.
  - Use a mixed workload: multiple reader coroutines, multiple writer coroutines, and if useful a small number of timeout-based try-lock coroutines.
  - Maintain simple shared invariants that can detect corruption quickly, such as paired counters or monotonic write epochs observed under read lock.
  - Add the new executable to `benchmark_test/CMakeLists.txt`.

  **Must NOT do**:
  - Do not turn the fatigue program into a unit test.
  - Do not omit periodic stdout visibility that helps identify livelock or stall.

  **Recommended Agent Profile**:
  - **Category**: `quick`
    - Reason: standalone stress harness built from an existing local pattern.
  - **Skills**: `test-driven-development`
    - `test-driven-development`: keeps the fatigue program centered on observable invariants rather than random looping.
  - **Skills Evaluated but Omitted**:
    - `systematic-debugging`: not the best primary fit for a harness patterned after existing fatigue binaries.

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2 (with Task 6)
  - **Blocks**: Final verification
  - **Blocked By**: 3

  **References**:
  - `benchmark_test/fatigue_comutex.cc:1-60` - canonical local fatigue executable shape.
  - `benchmark_test/CMakeLists.txt:19-44` - existing benchmark target declarations to mirror.
  - `debug/Debug_co_rwmutex.cc:7-67` - rwmutex-specific mixed read/write load idea to adapt.

  **Acceptance Criteria**:
  - [ ] `benchmark_test/fatigue_corwmutex.cc` exists and is wired into benchmark CMake.
  - [ ] `cmake --build build --target fatigue_corwmutex` succeeds.
  - [ ] The program emits periodic status and sustains load without assertion failure.

  **QA Scenarios**:
  ```text
  Scenario: Fatigue target builds successfully
    Tool: Bash
    Preconditions: File and CMake entry added
    Steps:
      1. Run `cmake --build build --target fatigue_corwmutex`.
      2. Confirm target links successfully.
    Expected Result: Build completes with a runnable fatigue executable.
    Failure Indicators: Missing target, compile failure, or link failure.
    Evidence: .sisyphus/evidence/task-7-fatigue-build.txt

  Scenario: Fatigue program runs and preserves invariants
    Tool: Bash
    Preconditions: Fatigue binary built
    Steps:
      1. Run `./build/bin/benchmark_test/fatigue_corwmutex` under a bounded timeout wrapper.
      2. Observe periodic stdout updates during the run window.
      3. Confirm the process exits only because of the timeout wrapper, not assertion failure or crash.
    Expected Result: Program remains live, prints status, and does not violate invariants.
    Failure Indicators: Assertion failure, deadlock with no output, crash, or abnormal exit.
    Evidence: .sisyphus/evidence/task-7-fatigue-run.txt
  ```

- [ ] 8. Expand multi-reader/multi-writer bounded stress assertions in Test_co_rwmutex

  **What to do**:
  - Strengthen the existing mixed concurrency test in `unit_test/Test_co_rwmutex.cc` or split it into narrower bounded stress cases.
  - Cover at least these invariants:
    - readers never observe an active writer critical section,
    - writers never overlap,
    - both reader and writer populations make progress during the observation window,
    - try-lock paths do not destabilize long-running correctness.
  - Use multiple writers, not just one writer, to validate writer exclusivity under contention.

  **Must NOT do**:
  - Do not rely on a single weak boolean if stronger observable invariants can be asserted.
  - Do not make the unit suite effectively infinite; keep it bounded for CTest.

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
    - Reason: bounded concurrency stress tests need careful synchronization and assertions.
  - **Skills**: `test-driven-development`, `systematic-debugging`
    - `test-driven-development`: drives explicit invariants first.
    - `systematic-debugging`: helps eliminate flaky concurrency assumptions.
  - **Skills Evaluated but Omitted**:
    - `quick`: too weak a fit for concurrency stress validation.

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2 (with Tasks 6, 7 where practical)
  - **Blocks**: Final verification
  - **Blocked By**: 2, 3, 5

  **References**:
  - `unit_test/Test_co_rwmutex.cc:77-124` - current multi-coroutine test that needs stronger invariants and possibly multiple writers.
  - `debug/Debug_co_rwmutex.cc:15-53` - a broad mixed reader/writer spawning pattern.
  - `benchmark_test/fatigue_comutex.cc:11-50` - repeated invariant checks in hot loops.

  **Acceptance Criteria**:
  - [ ] At least one bounded unit test uses more than one writer.
  - [ ] The suite asserts both safety (no overlap corruption) and liveness-style signals (progress counters advance).
  - [ ] `ctest -R Test_co_rwmutex --output-on-failure` stays bounded and passes.

  **QA Scenarios**:
  ```text
  Scenario: Multi-reader/multi-writer stress case passes in bounded unit mode
    Tool: Bash (ctest)
    Preconditions: Bounded stress case added
    Steps:
      1. Run `ctest --test-dir build -R Test_co_rwmutex --output-on-failure`.
      2. Confirm the stress case completes within the normal test run.
      3. Confirm no assertion reports reader/writer overlap or stalled progress.
    Expected Result: Stress case passes with observable progress from both populations.
    Failure Indicators: Test timeout, assertion failure, or one population never progressing.
    Evidence: .sisyphus/evidence/task-8-stress-pass.txt

  Scenario: Multiple writers remain mutually exclusive
    Tool: Bash (ctest)
    Preconditions: Stress case tracks active writer count or equivalent invariant
    Steps:
      1. Run `ctest --test-dir build -R Test_co_rwmutex --output-on-failure`.
      2. Confirm no assertion detects simultaneous writers in the critical section.
    Expected Result: Writer exclusivity holds under multi-writer contention.
    Failure Indicators: Any assertion showing concurrent writer presence.
    Evidence: .sisyphus/evidence/task-8-writer-exclusive.txt
  ```


---

## Final Verification Wave

- [ ] F1. **Plan Compliance Audit** — `oracle`
  Verify that CoRWMutex exposes explicit read/write unlock APIs, timeout try-lock APIs, and preserves write-priority semantics. Confirm fatigue target exists and unit tests cover the planned cases.

- [ ] F2. **Code Quality Review** — `unspecified-high`
  Build changed targets, inspect for dead code, queue misuse, stale compatibility shims, and timeout-path mistakes.

- [ ] F3. **Real QA Execution** — `unspecified-high`
  Run `Test_co_rwmutex`, build and execute `fatigue_corwmutex`, and capture evidence for both normal and timeout behavior.

- [ ] F4. **Scope Fidelity Check** — `deep`
  Ensure the diff stays limited to rwmutex behavior/API/tests/build wiring and does not spill into generic lock abstractions.

---

## Commit Strategy

- **1**: `refactor(coroutine): split rwmutex unlock paths and fix wakeup ordering`
- **2**: `test(coroutine): expand rwmutex coverage and add fatigue target`

---

## Success Criteria

### Verification Commands
```bash
cmake -S . -B build -DNEED_TEST=ON -DNEED_BENCHMARK=ON
cmake --build build --target Test_co_rwmutex fatigue_corwmutex
ctest --test-dir build -R Test_co_rwmutex --output-on-failure
./build/bin/benchmark_test/fatigue_corwmutex
```

### Final Checklist
- [ ] All planned rwmutex API changes are present
- [ ] Write-priority behavior preserved
- [ ] Timeout try-lock behavior covered by tests
- [ ] Fatigue target builds and runs
- [ ] No unrelated lock abstraction refactor introduced
