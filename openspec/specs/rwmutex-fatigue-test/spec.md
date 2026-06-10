## ADDED Requirements

### Requirement: Fatigue program exercises mixed reader/writer load
The system SHALL include a standalone fatigue program (`fatigue_corwmutex`) that spawns multiple reader and writer coroutines operating on a shared `CoRWMutex` instance. The program MUST run indefinitely, periodically outputting status, and MUST NOT trigger assertion failures or deadlocks.

#### Scenario: Fatigue program starts and sustains load
- **WHEN** `fatigue_corwmutex` is executed
- **THEN** the program spawns reader and writer coroutines, prints periodic status output, and continues running without assertion failure

### Requirement: Fatigue program asserts reader/writer invariants
The fatigue program SHALL include assertions that verify: (a) no reader observes an active writer section, (b) no two writers overlap in the critical section, and (c) both reader and writer populations make observable progress.

#### Scenario: Reader invariant holds under concurrent writers
- **WHEN** fatigue program runs with mixed reader/writer workoad
- **THEN** no assertion fires indicating a reader observed an active writer

#### Scenario: Writer exclusivity holds under concurrent writers
- **WHEN** fatigue program runs with multiple writer coroutines
- **THEN** no assertion fires indicating two writers were in the critical section simultaneously

### Requirement: Fatigue program build wiring
The fatigue program MUST be registered as a CMake target in `benchmark_test/CMakeLists.txt` following the existing pattern (e.g., `fatigue_comutex`). The target SHALL link against `bbt_coroutine`.

#### Scenario: Fatigue target builds
- **WHEN** `cmake --build build --target fatigue_corwmutex` is invoked
- **THEN** the program compiles and links successfully
