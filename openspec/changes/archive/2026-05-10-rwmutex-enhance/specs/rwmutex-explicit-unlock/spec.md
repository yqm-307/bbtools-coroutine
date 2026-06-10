## ADDED Requirements

### Requirement: Reader unlock via RUnLock
The system SHALL provide `RUnLock()` for releasing a read lock previously acquired via `RLock()`. `RUnLock()` MUST decrement the read-holder count and update the lock state before waking any waiters.

#### Scenario: Last reader unlocks with no waiters
- **WHEN** a single reader holds the lock and calls `RUnLock()`
- **THEN** the lock state transitions to FREE and no waiters are notified

#### Scenario: Non-last reader unlocks
- **WHEN** reader A and reader B hold the lock, and reader A calls `RUnLock()`
- **THEN** the read-holder count decrements, the lock remains RLOCKED, and no waiters are woken

#### Scenario: Last reader unlocks with waiting writer
- **WHEN** two readers hold the lock, a writer is waiting, and the last reader calls `RUnLock()`
- **THEN** the lock transitions to FREE and one writer from the write queue is woken

### Requirement: Writer unlock via WUnLock
The system SHALL provide `WUnLock()` for releasing a write lock previously acquired via `WLock()`. `WUnLock()` MUST set the lock state to FREE before waking waiters.

#### Scenario: Writer unlocks with no waiters
- **WHEN** a writer holds the lock and calls `WUnLock()`
- **THEN** the lock state transitions to FREE and no waiters are notified

#### Scenario: Writer unlocks with waiting readers only
- **WHEN** a writer holds the lock, multiple readers are waiting, and calls `WUnLock()`
- **THEN** the lock transitions to FREE and ALL waiting readers are woken

#### Scenario: Writer unlocks with waiting writers
- **WHEN** a writer holds the lock, another writer is waiting, and calls `WUnLock()`
- **THEN** the lock transitions to FREE and exactly one waiting writer is woken

### Requirement: No generic UnLock for rwmutex
The CoRWMutex public API SHALL NOT expose a generic `UnLock()` method. Read and write lock release MUST use `RUnLock()` and `WUnLock()` respectively.

#### Scenario: Compile error when calling UnLock on rwmutex
- **WHEN** code attempts to call `rwlock->UnLock()`
- **THEN** compilation fails with an error indicating the method does not exist
