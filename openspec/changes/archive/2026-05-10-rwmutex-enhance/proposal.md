## Why

`CoRWMutex` currently has correctness bugs in its unlock/wakeup ordering (notifies waiters before updating internal state), exposes only a generic `UnLock()` that masks read-lock vs write-lock semantics, and lacks non-blocking and timeout try-lock APIs. Its test coverage is thin—only basic reader/writer scenarios—with no fatigue validation. This change fixes the state machine, promotes the API, adds try-lock support, and raises confidence with stronger tests and a dedicated stress program.

## What Changes

- **BREAKING**: Replace unified `UnLock()` with explicit `RUnLock()` / `WUnLock()` for type-safe lock release
- Fix unlock/wakeup ordering: update internal state before notifying waiters
- Fix spurious writer wakeup when non-last reader unlocks
- Fix writer-release path to directly wake all queued readers instead of relying on chain-reaction
- Add `TryRLock()` / `TryRLock(int ms)` and `TryWLock()` / `TryWLock(int ms)` with timeout support
- Expand unit test coverage: write-priority enforcement, timeout semantics, multi-writer exclusivity
- Add `fatigue_corwmutex` benchmark program for sustained stress validation
- Maintain existing write-priority strategy (`m_has_wait_wlock`) unchanged

## Capabilities

### New Capabilities

- `rwmutex-explicit-unlock`: Reader and writer lock release must use type-specific `RUnLock()` and `WUnLock()` respectively, ensuring compile-time distinction between read-lock and write-lock release semantics.
- `rwmutex-try-lock`: Non-blocking and timeout-based lock acquisition for both readers and writers, returning consistent success/timeout/failure codes aligned with `CoMutex::TryLock`.
- `rwmutex-write-priority`: When a writer is queued, new readers must be blocked until the writer acquires and releases. Reader unlocks must only wake writers when the lock transitions to FREE.
- `rwmutex-fatigue-test`: A standalone stress program exercising mixed reader/writer workloads indefinitely with invariant assertions, providing confidence in lock correctness under sustained concurrency.

### Modified Capabilities

<!-- No existing rwmutex specs to modify -->

## Impact

- **Affected files**: `bbt/coroutine/sync/CoRWMutex.{hpp,cc}`, `unit_test/Test_co_rwmutex.cc`, `debug/Debug_co_rwmutex.cc`, `benchmark_test/fatigue_corwmutex.cc` (new), `benchmark_test/CMakeLists.txt`
- **APIs**: `UnLock()` removed from public surface; `RUnLock()`, `WUnLock()`, `TryRLock()`, `TryRLock(int)`, `TryWLock()`, `TryWLock(int)` added
- **Backward compatibility**: All repo-internal rwmutex call sites migrated to new API; no shim retained
- **No impact on**: `ICoLock`, `StdLockWapper`, other sync primitives (`CoMutex`, `CoCond`, `CoWaiter`)
