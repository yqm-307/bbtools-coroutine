---
name: managing-fatigue-tests
description: Use when creating new fatigue tests, registering them in CMakeLists/config, or starting fatigue monitoring. Covers the full lifecycle: write .cc → register → build → monitor → web dashboard.
---

# Managing Fatigue Tests

## Overview

Fatigue tests are long-running stress binaries for individual coroutine primitives (chan, mutex, copool, etc.). Each test lives in `benchmark_test/` as `fatigue_<name>.cc`, is built via CMake, and monitored through a Python web dashboard.

**Announce at start:** "I'm using the managing-fatigue-tests skill."

## When to Use

- Creating a new fatigue test for a coroutine primitive
- Registering a fatigue binary in CMakeLists.txt or the monitor config
- Starting the monitor server
- Understanding the web dashboard API

## Fatigue Test Lifecycle

```
write .cc  →  register in CMakeLists.txt  →  build  →  register in config  →  monitor
```

## Step 1: Write the Fatigue Test

**File:** `benchmark_test/fatigue_<name>.cc`

**Template:**

```cpp
#include <bbt/coroutine/coroutine.hpp>

int main()
{
    g_scheduler->Start();

    // Stress-test logic here — run indefinitely or for N hours.

    g_scheduler->Stop();
}
```

**Conventions:**
- Name files `fatigue_<component>.cc` (e.g., `fatigue_comutex.cc`)
- Use `g_scheduler->Start()` / `Stop()` as entry/exit guards
- Include `sleep(N)` or time-based loops for long-running tests
- Print periodic status via `printf()` so it's visible in the monitor's stdout log

**Existing examples:**

| File | What it stresses |
|------|-----------------|
| `fatigue_coroutine.cc` | 100k coroutine spawns/sec |
| `fatigue_chan.cc` | Channel read/write throughput |
| `fatigue_copool.cc` | 10 pools × 100k tasks per batch |
| `fatigue_cocond.cc` | 2000 coroutines with condition-variable notify/wait |
| `fatigue_comutex.cc` | 100+ coroutines contending on CoMutex |
| `fatigue_corwmutex.cc` | 100 readers + 5 writers on CoRWMutex |
| `fatigue_std_uniquelock.cc` | 1000 coroutines with std::unique_lock wrapper |

## Step 2: Register in CMakeLists.txt

**File:** `benchmark_test/CMakeLists.txt`

Add two lines per fatigue test:

```cmake
add_executable(fatigue_<name> fatigue_<name>.cc)
target_link_libraries(fatigue_<name> ${MY_LIBS})
```

Insert alongside the other fatigue entries (currently grouped together after the `set(MY_LIBS ...)` block).

## Step 3: Build

```bash
# From project root
./build.sh

# Or manually:
mkdir -p build && cd build
cmake -DNEED_BENCHMARK=ON ..
make
```

Binaries output to: `build/bin/benchmark_test/fatigue_<name>`

## Step 4: Register in Monitor Config

**File:** `scripts/fatigue_monitor/fatigue_config.yaml`

Add an entry:

```yaml
  - name: fatigue_<name>
    binary: fatigue_<name>
```

To skip a test, set `enabled: false`:

```yaml
  - name: fatigue_<name>
    binary: fatigue_<name>
    enabled: false
```

## Step 5: Start Monitor

```bash
# Install deps (once)
pip install -r scripts/fatigue_monitor/requirements.txt

# Start monitor + dashboard
cd scripts/fatigue_monitor
python3 fatigue_monitor.py [--port 8888] [--host 0.0.0.0]
```

**CLI options:**

| Flag | Default | Description |
|------|---------|-------------|
| `--config` | `fatigue_config.yaml` | Config file path |
| `--host` | `0.0.0.0` | Bind address |
| `--port` | `8888` | Bind port |
| `--no-launch` | off | Attach to existing PIDs instead of launching |

## Web Dashboard API

Base URL: `http://<host>:<port>`

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Dashboard HTML page (single-file SPA with Chart.js) |
| `/ws` | WebSocket | Real-time data push: `{"ts": <unix_ts>, "data": [{"name":"...","pid":N,"alive":bool,"cpu":N,"mem":N,"time":ts}, ...]}` |
| `/history` | GET | JSON: full history for all processes since monitor start |
| `/csv` | GET | CSV download of all collected metrics |

**WebSocket message format (server → client):**

```json
{
  "ts": 1715827201.234,
  "data": [
    {
      "name": "fatigue_coroutine",
      "pid": 12345,
      "alive": true,
      "cpu": 12.5,
      "mem": 34.2,
      "time": 1715827201.234
    }
  ]
}
```

When all processes exit, a final `{"done": true}` message is sent.

**CSV columns:** `process, pid, timestamp, cpu_percent, mem_mb`

## Shutdown

Press `Ctrl+C` in the monitor terminal. All launched processes receive `SIGTERM` then `SIGKILL` (3s grace). Collected data auto-saves to `fatigue_results.csv` in the project root.
