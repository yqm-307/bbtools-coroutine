## ADDED Requirements

### Requirement: Non-blocking try-read-lock
The system SHALL provide `TryRLock()` that attempts to acquire a read lock without blocking. It MUST return `0` on success and `-1` if the lock cannot be immediately acquired.

#### Scenario: TryRLock succeeds when lock is free
- **WHEN** no coroutine holds the lock and `TryRLock()` is called
- **THEN** the caller acquires a read lock and the method returns `0`

#### Scenario: TryRLock succeeds when another reader holds the lock
- **WHEN** reader A holds the lock via `RLock()` and `TryRLock()` is called by reader B
- **THEN** reader B acquires a read lock and the method returns `0`

#### Scenario: TryRLock fails when writer holds the lock
- **WHEN** a writer holds the lock and `TryRLock()` is called
- **THEN** the caller does not acquire the lock and the method returns `-1`

#### Scenario: TryRLock fails when writer is waiting (write-priority)
- **WHEN** a reader holds the lock, a writer is queued, and `TryRLock()` is called
- **THEN** the caller does not acquire the lock and the method returns `-1`

### Requirement: Timeout try-read-lock
The system SHALL provide `TryRLock(int ms)` that attempts to acquire a read lock, waiting up to the specified timeout. It MUST return `0` on success, `1` on timeout, and `-1` on error.

#### Scenario: TryRLock with timeout succeeds before deadline
- **WHEN** a writer holds the lock, `TryRLock(500)` is called, and the writer unlocks within 100ms
- **THEN** the caller acquires the read lock and the method returns `0`

#### Scenario: TryRLock with timeout expires
- **WHEN** a writer holds the lock and `TryRLock(50)` is called
- **THEN** the caller does not acquire the lock and the method returns `1` after the timeout elapses

### Requirement: Non-blocking try-write-lock
The system SHALL provide `TryWLock()` that attempts to acquire a write lock without blocking. It MUST return `0` on success and `-1` if the lock cannot be immediately acquired.

#### Scenario: TryWLock succeeds when lock is free
- **WHEN** no coroutine holds the lock and `TryWLock()` is called
- **THEN** the caller acquires a write lock and the method returns `0`

#### Scenario: TryWLock fails when reader holds the lock
- **WHEN** a reader holds the lock and `TryWLock()` is called
- **THEN** the caller does not acquire the lock and the method returns `-1`

#### Scenario: TryWLock fails when writer holds the lock
- **WHEN** a writer holds the lock and `TryWLock()` is called
- **THEN** the caller does not acquire the lock and the method returns `-1`

### Requirement: Timeout try-write-lock
The system SHALL provide `TryWLock(int ms)` that attempts to acquire a write lock, waiting up to the specified timeout. It MUST return `0` on success, `1` on timeout, and `-1` on error.

#### Scenario: TryWLock with timeout succeeds after readers release
- **WHEN** readers hold the lock, `TryWLock(500)` is called, and all readers unlock within 100ms
- **THEN** the caller acquires the write lock and the method returns `0`

#### Scenario: TryWLock with timeout expires
- **WHEN** a reader holds the lock and `TryWLock(50)` is called
- **THEN** the caller does not acquire the lock and the method returns `1` after the timeout elapses

### Requirement: Timeout does not corrupt wait queue
The system SHALL ensure that after a try-lock timeout, subsequent wakeups on the same queue remain correct. A timed-out waiter MUST be safely skipped during `_NotifyOne()` / `_NotifyAll()` traversal.

#### Scenario: Acquisition succeeds after a prior timeout
- **WHEN** `TryRLock(10)` times out in coroutine A, and later the lock becomes FREE
- **THEN** coroutine B can successfully acquire and release the lock via `RLock()` + `RUnLock()` without blocking or error
