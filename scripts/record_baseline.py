#!/usr/bin/env python3
"""
record_baseline.py — 记录 bbtools-coroutine 性能基线

用法:
  python3 scripts/record_baseline.py [--threads N] [--dur S] [--quick]
  python3 scripts/record_baseline.py --compare <baseline1> <baseline2>

输出: tests/baselines/<machine>/<timestamp>.json

契约参考: docs/testing-contract-spec.md §2.9
ADR:       docs/adr/0001-testing-contract.md D3 (per-machine baselines)
"""
import argparse
import json
import os
import platform
import re
import subprocess
import sys
import time
from datetime import datetime

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(PROJECT_DIR, "build")
BASELINE_DIR = os.path.join(PROJECT_DIR, "tests", "baselines")
UNIFIED_STRESS = os.path.join(BUILD_DIR, "bin", "benchmark_test", "unified_stress")
MICRO_CO_SWITCH = os.path.join(BUILD_DIR, "bin", "benchmark_test", "micro_co_switch")

MODULES = ["comutex", "corwmutex", "cocond", "chan", "copool", "coroutine"]

# Regression thresholds per ADR D6
THRESHOLD_INTERCEPT = 10   # >10% → intercept PR
THRESHOLD_ESCALATE = 20    # >20% → escalate to incident
THRESHOLD_COCOND = 30      # CoCond exception: >30%


def get_git_commit():
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=PROJECT_DIR, text=True
        ).strip()
    except Exception:
        return "unknown"


def get_machine_id():
    """Generate a stable machine identifier."""
    try:
        cpu = subprocess.check_output(
            "cat /proc/cpuinfo | grep 'model name' | head -1 | cut -d: -f2",
            shell=True, text=True
        ).strip()
    except Exception:
        cpu = platform.processor()
    mem = "unknown"
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    mem = line.split()[1] + "kB"
                    break
    except Exception:
        pass
    hostname = platform.node()
    return f"{hostname} | {cpu} | {mem}"


def run_module_benchmark(module, threads, duration):
    """Run unified_stress for one module and parse results."""
    env = os.environ.copy()
    env["FATIGUE_INTERVAL"] = "10"

    cmd = [UNIFIED_STRESS, f"--module={module}", f"--threads={threads}",
           str(duration), "0", "0"]
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=duration + 30,
            cwd=BUILD_DIR, env=env
        )
    except subprocess.TimeoutExpired:
        return None

    # Parse last FATIGUE_METRIC for this module
    metrics = None
    for line in result.stdout.split("\n"):
        if f'"name":"{module}"' in line and "FATIGUE_METRIC:" in line:
            try:
                json_str = line.split("FATIGUE_METRIC:", 1)[1]
                metrics = json.loads(json_str)
            except json.JSONDecodeError:
                continue

    if metrics is None or metrics.get("elapsed_s", 0) < 10:
        return None

    elapsed = metrics["elapsed_s"]
    return {
        "ops_total": metrics["ops_total"],
        "ops_per_s": round(metrics["ops_total"] / elapsed, 1),
        "errors": metrics["errors"],
        "lock_ops": metrics.get("lock_ops", 0),
        "trylock_timeout": metrics.get("trylock_timeout", 0),
        "lock_avg_us": metrics.get("lock_avg_us", 0),
        "wlock_ops": metrics.get("wlock_ops", 0),
        "wlock_avg_us": metrics.get("wlock_avg_us", 0),
        "rlock_ops": metrics.get("rlock_ops", 0),
        "cond_waits": metrics.get("cond_waits", 0),
        "cond_signals": metrics.get("cond_signals", 0),
        "cond_avg_us": metrics.get("cond_avg_us", 0),
        "chan_reads": metrics.get("chan_reads", 0),
        "chan_writes": metrics.get("chan_writes", 0),
        "pool_tasks": metrics.get("pool_tasks", 0),
        "co_spawns": metrics.get("co_spawns", 0),
    }


def run_micro_benchmark(threads):
    """Run micro_co_switch and parse results."""
    if not os.path.exists(MICRO_CO_SWITCH):
        print("  [WARN] micro_co_switch not found, skipping switch latency")
        return None

    cmd = [MICRO_CO_SWITCH, "1000000", str(threads)]
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=30, cwd=BUILD_DIR
        )
    except subprocess.TimeoutExpired:
        return None

    for line in result.stdout.split("\n"):
        if "BENCH_RESULT:" in line:
            try:
                json_str = line.split("BENCH_RESULT:", 1)[1]
                return json.loads(json_str)
            except json.JSONDecodeError:
                continue
    return None


def record_baseline(threads, duration, quick=False):
    """Record a full baseline for all modules."""
    if not os.path.exists(UNIFIED_STRESS):
        print(f"ERROR: unified_stress not found at {UNIFIED_STRESS}")
        print("Build first: cd build && cmake .. -DNEED_BENCHMARK=ON && ninja")
        sys.exit(1)

    machine = get_machine_id()
    machine_slug = re.sub(r'[^\w\-]', '_', platform.node())
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    commit = get_git_commit()

    baseline = {
        "timestamp": datetime.now().isoformat(),
        "git_commit": commit,
        "machine": machine,
        "threads": threads,
        "quick": quick,
        "modules": {},
        "micro_bench": {},
    }

    print(f"Recording baseline: commit={commit}, threads={threads}, dur={duration}s")
    print(f"Machine: {machine}")

    # Per-module benchmarks
    for module in MODULES:
        dur = min(duration, 60) if quick else duration
        print(f"  [{module}] running {dur}s...", end=" ", flush=True)
        t0 = time.time()
        result = run_module_benchmark(module, threads, dur)
        elapsed = time.time() - t0
        if result:
            baseline["modules"][module] = result
            print(f"✅ {result['ops_per_s']:.0f} ops/s ({elapsed:.1f}s)")
        else:
            baseline["modules"][module] = {"error": "no_data"}
            print(f"❌ no data ({elapsed:.1f}s)")

    # Micro-benchmarks
    print("  [micro_co_switch] running...", end=" ", flush=True)
    micro = run_micro_benchmark(threads)
    if micro:
        baseline["micro_bench"] = micro
        print(f"✅ yield={micro.get('yield_roundtrip_avg_ns', 0):.0f}ns")
    else:
        baseline["micro_bench"] = {"error": "no_data"}
        print("❌ no data")

    # Write baseline
    os.makedirs(os.path.join(BASELINE_DIR, machine_slug), exist_ok=True)
    path = os.path.join(BASELINE_DIR, machine_slug, f"{ts}_{commit}.json")
    with open(path, "w") as f:
        json.dump(baseline, f, indent=2)
    print(f"\nBaseline saved: {path}")
    return path


def compare_baselines(path1, path2):
    """Compare two baselines and report regressions."""
    with open(path1) as f:
        b1 = json.load(f)
    with open(path2) as f:
        b2 = json.load(f)

    print(f"Comparing baselines:")
    print(f"  Old: {os.path.basename(path1)} (commit={b1.get('git_commit', '?')})")
    print(f"  New: {os.path.basename(path2)} (commit={b2.get('git_commit', '?')})")
    print()

    verdict = "PASS"
    issues = []

    # Header
    print(f"{'Module':<12} {'Old ops/s':>10} {'New ops/s':>10} {'Delta%':>8} {'Threshold':>10} {'Verdict':>8}")
    print("-" * 68)

    for module in MODULES:
        old_mod = b1.get("modules", {}).get(module, {})
        new_mod = b2.get("modules", {}).get(module, {})

        if old_mod.get("error") or new_mod.get("error"):
            print(f"{module:<12} {'N/A':>10} {'N/A':>10} {'N/A':>8} {'N/A':>10} {'SKIP':>8}")
            continue

        old_ops = old_mod.get("ops_per_s", 0)
        new_ops = new_mod.get("ops_per_s", 0)

        if old_ops == 0:
            print(f"{module:<12} {old_ops:>10.0f} {new_ops:>10.0f} {'N/A':>8} {'N/A':>10} {'SKIP':>8}")
            continue

        delta_pct = (new_ops - old_ops) / old_ops * 100

        # Determine threshold
        threshold = THRESHOLD_COCOND if module == "cocond" else THRESHOLD_INTERCEPT
        escalate = THRESHOLD_ESCALATE if module != "cocond" else 99

        if delta_pct <= -escalate:
            status = "❌ FAIL"
            verdict = "FAIL"
            issues.append(f"{module}: {delta_pct:+.1f}% (>{escalate}%)")
        elif delta_pct <= -threshold:
            status = "⚠️ WARN"
            if verdict == "PASS":
                verdict = "WARN"
            issues.append(f"{module}: {delta_pct:+.1f}% (>{threshold}%)")
        else:
            status = "✅ OK"

        print(f"{module:<12} {old_ops:>10.0f} {new_ops:>10.0f} {delta_pct:>+7.1f}% {threshold:>9}% {status:>8}")

    # Also compare lock latency
    if b1.get("modules", {}).get("comutex", {}).get("lock_avg_us", 0) > 0:
        print()
        print(f"{'Module':<12} {'Old avg_us':>10} {'New avg_us':>10} {'Delta%':>8} {'Verdict':>8}")
        print("-" * 56)
        for module in ["comutex", "corwmutex", "cocond"]:
            old_mod = b1.get("modules", {}).get(module, {})
            new_mod = b2.get("modules", {}).get(module, {})
            old_lat = old_mod.get("lock_avg_us", 0) or old_mod.get("wlock_avg_us", 0) or old_mod.get("cond_avg_us", 0)
            new_lat = new_mod.get("lock_avg_us", 0) or new_mod.get("wlock_avg_us", 0) or new_mod.get("cond_avg_us", 0)
            if old_lat > 0 and new_lat > 0:
                delta_lat = (new_lat - old_lat) / old_lat * 100
                status_lat = "⚠️" if delta_lat > 20 else "✅"
                print(f"{module:<12} {old_lat:>10.1f} {new_lat:>10.1f} {delta_lat:>+7.1f}% {status_lat:>8}")

    # Micro-bench comparison
    if b1.get("micro_bench") and b2.get("micro_bench"):
        mb1 = b1["micro_bench"]
        mb2 = b2["micro_bench"]
        if "yield_roundtrip_avg_ns" in mb1 and "yield_roundtrip_avg_ns" in mb2:
            old_sw = mb1["yield_roundtrip_avg_ns"]
            new_sw = mb2["yield_roundtrip_avg_ns"]
            if old_sw > 0:
                delta_sw = (new_sw - old_sw) / old_sw * 100
                sw_status = "⚠️" if delta_sw > 50 else "✅"
                print(f"\n{'co_switch':<12} {old_sw:>10.0f} {new_sw:>10.0f} {delta_sw:>+7.1f}% {sw_status:>8} (ns, <50%=OK per spec §2.2)")

    print()
    print(f"Overall verdict: {verdict}")
    if issues:
        print("Issues:")
        for issue in issues:
            print(f"  - {issue}")

    return 0 if verdict == "PASS" else 1


def latest_baseline(machine_slug=None):
    """Find the most recent baseline file."""
    if machine_slug is None:
        machine_slug = re.sub(r'[^\w\-]', '_', platform.node())
    path = os.path.join(BASELINE_DIR, machine_slug)
    if not os.path.isdir(path):
        return None
    files = sorted([f for f in os.listdir(path) if f.endswith(".json")], reverse=True)
    return os.path.join(path, files[0]) if files else None


def main():
    parser = argparse.ArgumentParser(description="bbtools-coroutine performance baseline tool")
    sub = parser.add_subparsers(dest="command")

    record = sub.add_parser("record", help="Record a new baseline")
    record.add_argument("--threads", type=int, default=2, help="Processer threads per module")
    record.add_argument("--dur", type=int, default=60, help="Duration per module (seconds)")
    record.add_argument("--quick", action="store_true", help="Quick mode (max 60s per module)")

    compare = sub.add_parser("compare", help="Compare two baselines")
    compare.add_argument("baseline1", nargs="?", help="Old baseline path (default: penultimate)")
    compare.add_argument("baseline2", nargs="?", help="New baseline path (default: latest)")
    compare.add_argument("--latest-only", action="store_true", help="Compare latest two baselines")

    args = parser.parse_args()

    if args.command == "record":
        record_baseline(args.threads, args.dur, args.quick)

    elif args.command == "compare":
        machine_slug = re.sub(r'[^\w\-]', '_', platform.node())
        if args.latest_only or (not args.baseline1 and not args.baseline2):
            # Auto-detect latest two
            base_dir = os.path.join(BASELINE_DIR, machine_slug)
            if not os.path.isdir(base_dir):
                print(f"No baselines found in {base_dir}")
                sys.exit(1)
            files = sorted([f for f in os.listdir(base_dir) if f.endswith(".json")])
            if len(files) < 2:
                print(f"Need at least 2 baselines, found {len(files)}")
                sys.exit(1)
            args.baseline2 = os.path.join(base_dir, files[-1])
            args.baseline1 = os.path.join(base_dir, files[-2])

        if not args.baseline1:
            args.baseline1 = latest_baseline(machine_slug)
        if not args.baseline2:
            args.baseline2 = latest_baseline(machine_slug)

        if not args.baseline1 or not args.baseline2:
            print("Could not find baselines. Run 'record' first.")
            sys.exit(1)
        if not os.path.exists(args.baseline1):
            print(f"Baseline 1 not found: {args.baseline1}")
            sys.exit(1)
        if not os.path.exists(args.baseline2):
            print(f"Baseline 2 not found: {args.baseline2}")
            sys.exit(1)

        sys.exit(compare_baselines(args.baseline1, args.baseline2))

    else:
        parser.print_help()


if __name__ == "__main__":
    main()
