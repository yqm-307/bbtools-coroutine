## ADDED Requirements

### Requirement: New readers blocked while writer is queued
When a writer is waiting for the lock (`m_has_wait_wlock == true`), the system SHALL prevent new readers from acquiring a read lock. New readers MUST wait until the queued writer(s) have acquired and released the lock.

#### Scenario: Late reader blocked by waiting writer
- **WHEN** reader A holds the lock, writer W is queued, and reader B attempts `RLock()`
- **THEN** reader B suspends (does not acquire the lock) until writer W acquires and releases

#### Scenario: Late reader blocked by waiting writer via TryRLock
- **WHEN** reader A holds the lock, writer W is queued, and reader B attempts `TryRLock()`
- **THEN** `TryRLock()` returns `-1` immediately

### Requirement: Reader unlock preserves write-priority
When a non-last reader releases the lock, the system SHALL NOT wake any waiting writer. A waiting writer SHALL only be woken when the lock state transitions to FREE.

#### Scenario: Non-last reader unlock does not wake writer
- **WHEN** readers A and B hold the lock, writer W is waiting, and reader A calls `RUnLock()`
- **THEN** writer W remains waiting (is NOT woken) until reader B calls `RUnLock()`

#### Scenario: Last reader unlock wakes writer
- **WHEN** reader A holds the lock, writer W is waiting, and reader A calls `RUnLock()`
- **THEN** the lock transitions to FREE and writer W is woken and acquires the write lock

### Requirement: Writer unlock prefers waiting writers over readers
When a writer releases the lock and there are waiting writers, the system SHALL wake exactly one waiting writer, not any readers. This preserves the write-priority chain.

#### Scenario: Writer handoff to next writer
- **WHEN** writer W1 holds the lock, writer W2 is waiting, and W1 calls `WUnLock()`
- **THEN** writer W2 is woken and acquires the write lock before any waiting readers
