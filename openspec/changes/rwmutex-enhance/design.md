## Context

`CoRWMutex` is a coroutine-level read-write lock in the bbt-coroutine library. It uses `CoWaiter` for wait queues (unlike `CoMutex` which uses `CoPollEvent` directly). The current implementation has three interrelated bugs in the unlock/wakeup path, exposes a generic `UnLock()` that conflates reader and writer release, and offers no try-lock semantics.

The existing `CoWaiter` API already supports `WaitWithTimeoutAndCallback`, providing the infrastructure for timeout try-lock. `CoMutex::TryLock(int ms)` provides the return-value convention to follow.

## Goals / Non-Goals

**Goals:**
- Fix the unlock-before-notify ordering bug
- Prevent spurious writer wakeup when non-last reader unlocks
- Wake all readers directly when a writer unlocks (no chaining)
- Split `UnLock()` into `RUnLock()` and `WUnLock()` for type-safe unlock
- Add `TryRLock()`, `TryRLock(int ms)`, `TryWLock()`, `TryWLock(int ms)`
- Preserve existing write-priority strategy

**Non-Goals:**
- Lock upgrade (R→W) or downgrade (W→R)
- Reentrant/multi-acquire semantics
- Fairness mode switching (always write-priority)
- Integrating `CoRWMutex` into `ICoLock` / `StdLockWapper`
- Refactoring other sync primitives

## Decisions

### D1: Write-priority strategy via `m_has_wait_wlock` (preserved)

**Choice**: Keep the existing mechanism where `m_has_wait_wlock` is set when a writer enters the wait queue, and RLock checks this flag to block incoming readers.

**Rationale**: User confirmed write-priority is the intended design. This prevents writer starvation under high read concurrency. No change to the strategy itself.

**Alternatives considered**: Reader-priority (would starve writers), fair queue (more complex, slower for read-heavy workloads). Rejected as out of scope.

### D2: Notify-after-state-update ordering

**Choice**: In both `RUnLock()` and `WUnLock()`, update internal state (`m_status`, `m_rlock_hold_num`) before calling any `Notify*` method.

**Rationale**: The original code called `_NotifyOne()` before updating state. In a multi-threaded scheduler, the woken coroutine could observe stale state. Correct ordering eliminates this class of bug entirely.

**Alternatives considered**: Keep current order but add double-check (fragile, wastes wakeups). Rejected.

### D3: Direct NotifyAll readers on writer release

**Choice**: When a writer calls `WUnLock()` and no writers are queued, wake all waiting readers via `_NotifyAll(true)` rather than waking one reader and relying on chain-reaction.

**Rationale**: Eliminates chain-reaction latency and avoids a scenario where a single-reader wakeup never triggers the cascade (e.g., if the sole woken reader quickly unlocks before re-checking `m_has_wait_wlock`). Simpler and more direct.

**Alternatives considered**: Keep chain-reaction (fragile, unnecessary latency). Rejected.

### D4: No backward-compatible `UnLock()` shim

**Choice**: Remove `UnLock()` from CoRWMutex's public API. All repo-internal call sites (`unit_test/Test_co_rwmutex.cc`, `debug/Debug_co_rwmutex.cc`) are migrated to `RUnLock()` / `WUnLock()`.

**Rationale**: A generic shim defeats the purpose of type-safe unlock. The class doesn't implement `ICoLock`, so there's no interface contract binding it to `UnLock()`. Migration scope is limited to rwmutex-specific files.

### D5: Timeout via `CoWaiter::WaitWithTimeoutAndCallback`

**Choice**: Use `CoWaiter`'s existing `WaitWithTimeoutAndCallback` in the try-lock timeout path. Return `0` on success, `1` on timeout, `-1` on error (matches `CoMutex::TryLock` convention).

**Rationale**: `CoWaiter` already wraps timeout + event registration. Reusing it avoids duplicating CoPollEvent timeout logic. The return convention is consistent with the rest of the sync subsystem.

**Risk**: `CoWaiter::Notify()` returns `-1` if the waiter timed out (`m_run_status != COND_WAIT`). The `_NotifyOne` loop must handle this: skip the stale waiter and try the next. Addressed in implementation.

## Risks / Trade-offs

| Risk | Mitigation |
|------|------------|
| Timed-out CoWaiter remains in queue; later `_NotifyOne` hits it and gets -1 return | `_NotifyOne()` / `_NotifyAll()` must pop-and-skip waiters whose `Notify()` returns -1, advancing to the next entry |
| `RUnLock()`/`WUnLock()` break external users depending on `UnLock()` | External users calling `UnLock()` on rwmutex get compile error with clear message pointing to new API; migration is mechanical |
| Write-priority can cause reader starvation under continuous write load | This is inherent to write-priority design and is the accepted trade-off; no mitigation planned |

## Open Questions

<!-- All outstanding decisions resolved during exploration phase -->
