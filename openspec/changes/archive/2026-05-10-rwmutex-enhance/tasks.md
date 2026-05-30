## 1. API Redesign (Foundation)

- [x] 1.1 Update `CoRWMutex.hpp`: replace `UnLock()` with `RUnLock()` / `WUnLock()`, add `TryRLock()` / `TryRLock(int ms)` / `TryWLock()` / `TryWLock(int ms)` declarations, add private timeout wait helpers
- [x] 1.2 Update `CoRWMutex.cc`: implement `_WaitRLockWithTimeout` and `_WaitWLockWithTimeout` helpers using `CoWaiter::WaitWithTimeoutAndCallback`
- [x] 1.3 Implement `TryRLock()` / `TryRLock(int ms)` in `CoRWMutex.cc`: non-blocking path checks state immediately, timeout path enters wait with deadline
- [x] 1.4 Implement `TryWLock()` / `TryWLock(int ms)` in `CoRWMutex.cc`: same pattern as `TryRLock` but for write-lock acquisition

## 2. Unlock/Wakeup Bug Fixes

- [x] 2.1 Rewrite `RUnLock()`: update state first (decrement `m_rlock_hold_num`, transition to FREE if last), then notify only when lock becomes FREE
- [x] 2.2 Rewrite `WUnLock()`: set `m_status = FREE` first, then wake one writer if queued, else wake all readers via `_NotifyAll(true)`
- [x] 2.3 Update `_NotifyOne()` and `_NotifyAll()` to handle stale/timed-out waiters: pop-and-skip entries whose `Notify()` returns `-1`
- [x] 2.4 Update `RLock()` to trigger `_NotifyAll(true)` only when the lock transitions from waiting to acquired (preserving existing logic)

## 3. API Migration (Repo-Internal Call Sites)

- [x] 3.1 Update `unit_test/Test_co_rwmutex.cc`: replace all `UnLock()` calls with `RUnLock()` (reader paths) or `WUnLock()` (writer paths)
- [x] 3.2 Update `debug/Debug_co_rwmutex.cc`: replace all `UnLock()` calls with `RUnLock()` / `WUnLock()` matching the lock type used

## 4. Unit Test Expansion

- [x] 4.1 Add test case for write-priority: verify queued writer blocks late-arriving readers
- [x] 4.2 Add test case for `TryRLock(int ms)` timeout under writer hold
- [x] 4.3 Add test case for `TryWLock(int ms)` timeout under reader hold
- [x] 4.4 Add test case for post-timeout recovery: verify later acquisition succeeds after timeout
- [x] 4.5 Strengthen multi-reader/multi-writer stress test: use multiple writers, assert exclusivity and reader safety, ensure bounded for CTest

## 5. Fatigue Test Program

- [x] 5.1 Create `benchmark_test/fatigue_corwmutex.cc` with mixed reader/writer coroutines, invariant assertions, and periodic status output
- [x] 5.2 Add `fatigue_corwmutex` target to `benchmark_test/CMakeLists.txt` linking against `bbt_coroutine`

## 6. Build and Integration Verification

- [x] 6.1 Build all changed targets: `Test_co_rwmutex`, `Debug_co_rwmutex`, `fatigue_corwmutex`
- [x] 6.2 Run `ctest --test-dir build -R Test_co_rwmutex --output-on-failure` and confirm all tests pass
- [x] 6.3 Run `fatigue_corwmutex` under bounded timeout wrapper and confirm no assertions fire
