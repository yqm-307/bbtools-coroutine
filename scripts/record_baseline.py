#!/usr/bin/env python3
"""
record_baseline.py — 记录/比较 bbtools-coroutine 性能基线（任务 4 统一 Schema 版）

用法:
  python3 scripts/record_baseline.py record [--threads N] [--dur S] [--quick]
  python3 scripts/record_baseline.py record --output <path> [--environment-file <path>]
  python3 scripts/record_baseline.py compare [<baseline1> <baseline2>] [--latest-only]

输出:
  record → 统一 Schema 基线 JSON（含环境指纹，perf_contract schema）；
           默认 tests/baselines/<machine>/<ts>_<commit>.json
  compare → 逐模块对比，阈值/判定复用 perf_contract（CoCond 放宽 30%）。

兼容性:
  - 旧格式基线（无 environment 指纹、ops_per_s 字段）可被读取：记录仍可
    比较吞吐（提示指纹缺失、比对未验证）；ci_perf_check 对无指纹基线
    一律 NO_COMPARABLE_BASELINE（不可信）。
  - 基线由本脚本显式记录（Layer 3 push main）；PR Job 的 ci_perf_check
    不隐式写基线。
"""
import argparse
import json
import os
import platform
import re
import sys
import time
from datetime import datetime
from pathlib import Path

_CI_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ci")
if _CI_DIR not in sys.path:
    sys.path.insert(0, _CI_DIR)

from ci_common import run_command, write_json_atomic  # noqa: E402
import perf_contract  # noqa: E402

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(PROJECT_DIR, "build")
BASELINE_DIR = os.path.join(PROJECT_DIR, "tests", "baselines")
UNIFIED_STRESS = os.path.join(BUILD_DIR, "bin", "benchmark_test", "unified_stress")
MICRO_CO_SWITCH = os.path.join(BUILD_DIR, "bin", "benchmark_test", "micro_co_switch")

MODULES = ["comutex", "corwmutex", "cocond", "chan", "copool", "coroutine"]
REPOSITORY = "bbtools-coroutine"

SEVERITY = {
    "FAIL": 5, "METRIC_INVALID": 4, "UNSTABLE": 3, "WARN": 2,
    "NO_COMPARABLE_BASELINE": 1, "PASS": 0,
}


def get_git_commit():
    result = run_command(["git", "rev-parse", "--short", "HEAD"],
                         cwd=Path(PROJECT_DIR))
    if result.category != "pass":
        return "unknown"
    return result.stdout.strip()


def get_machine_id():
    """Generate a stable machine identifier (console 展示用)."""
    try:
        cpu = run_command(
            ["sh", "-c", "grep 'model name' /proc/cpuinfo | head -1 | cut -d: -f2"],
            timeout=5)
        cpu_text = cpu.stdout.strip() if cpu.category == "pass" else ""
    except Exception:
        cpu_text = ""
    cpu = cpu_text or platform.processor()
    mem = "unknown"
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    mem = line.split()[1] + "kB"
                    break
    except Exception:
        pass
    return f"{platform.node()} | {cpu} | {mem}"


def _read_build_info():
    """从 CMakeCache 读取 Build Type 与 CXX 标志。"""
    cache = Path(BUILD_DIR) / "CMakeCache.txt"
    build_type = "Release"
    cmake_args = []
    if cache.exists():
        for line in cache.read_text().splitlines():
            if line.startswith("CMAKE_BUILD_TYPE:STRING="):
                build_type = line.split("=", 1)[1].strip()
            elif line.startswith("CMAKE_CXX_FLAGS:STRING="):
                flags = line.split("=", 1)[1].strip()
                if flags:
                    cmake_args = [flags]
    return build_type, cmake_args


def collect_environment(threads, duration, environment_file=None, quick=False):
    """环境指纹：优先外部文件，否则本机采集（durations 用实际时长，不硬编码）。"""
    if environment_file:
        with open(environment_file) as f:
            return json.load(f)
    build_type, cmake_args = _read_build_info()
    effective = min(duration, 60) if quick else duration
    return perf_contract.collect_environment_fingerprint(
        threads=threads, modules=MODULES,
        durations=[effective] * len(MODULES),
        build_type=build_type, cmake_args=cmake_args,
    )


def run_module_benchmark(module, threads, duration):
    """运行 unified_stress 单模块并解析为统一模块指标；失败返回 None。"""
    env = os.environ.copy()
    env["FATIGUE_INTERVAL"] = "10"

    cmd = [UNIFIED_STRESS, f"--module={module}", f"--threads={threads}",
           str(duration), "0", "0"]
    result = run_command(cmd, cwd=Path(BUILD_DIR), env=env,
                         timeout=duration + 30)
    if result.category == "timeout":
        return None

    metrics = None
    for line in result.stdout.split("\n"):
        if "FATIGUE_METRIC:" not in line:
            continue
        try:
            json_str = line.split("FATIGUE_METRIC:", 1)[1].strip()
            candidate = json.loads(json_str)
        except json.JSONDecodeError:
            continue
        if not isinstance(candidate, dict) or candidate.get("name") != module:
            continue
        metrics = candidate

    if metrics is None:
        return None

    normalized = perf_contract.normalize_module_metrics(metrics)
    if normalized is None or normalized["elapsed_s"] < 10:
        return None
    return normalized


def run_micro_benchmark(threads):
    """运行 micro_co_switch 并解析结果。"""
    if not os.path.exists(MICRO_CO_SWITCH):
        print("  [WARN] micro_co_switch not found, skipping switch latency")
        return None
    cmd = [MICRO_CO_SWITCH, "1000000", str(threads)]
    result = run_command(cmd, cwd=Path(BUILD_DIR), timeout=30)
    if result.category != "pass":
        return None
    for line in result.stdout.split("\n"):
        if "BENCH_RESULT:" in line:
            try:
                json_str = line.split("BENCH_RESULT:", 1)[1]
                return json.loads(json_str)
            except json.JSONDecodeError:
                continue
    return None


def record_baseline(threads, duration, quick=False, output=None,
                    environment_file=None):
    """记录统一 Schema 基线：环境指纹 + 全模块指标 + micro_bench。"""
    if not os.path.exists(UNIFIED_STRESS):
        print(f"ERROR: unified_stress not found at {UNIFIED_STRESS}")
        print("Build first: cd build && cmake .. -DNEED_BENCHMARK=ON && ninja")
        sys.exit(1)

    machine = get_machine_id()
    machine_slug = re.sub(r'[^\w\-]', '_', platform.node())
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    commit = get_git_commit()
    env_fp = collect_environment(threads, duration, environment_file, quick)

    modules = {}
    print(f"Recording baseline: commit={commit}, threads={threads}, dur={duration}s")
    print(f"Machine: {machine}")
    print(f"Agent: {env_fp.get('agent')} | cpu: {env_fp.get('cpu_model')}")

    for module in MODULES:
        dur = min(duration, 60) if quick else duration
        print(f"  [{module}] running {dur}s...", end=" ", flush=True)
        t0 = time.time()
        result = run_module_benchmark(module, threads, dur)
        elapsed = time.time() - t0
        if result:
            modules[module] = result
            print(f"✅ {result['ops_per_sec']:.0f} ops/s ({elapsed:.1f}s)")
        else:
            modules[module] = {"error": "no_data"}
            print(f"❌ no data ({elapsed:.1f}s)")

    print("  [micro_co_switch] running...", end=" ", flush=True)
    micro = run_micro_benchmark(threads)
    if micro:
        print(f"✅ yield={micro.get('yield_roundtrip_avg_ns', 0):.0f}ns")
    else:
        micro = {"error": "no_data"}
        print("❌ no data")

    verdict = "PASS" if not any(
        m.get("error") for m in modules.values()) else "FAIL"
    baseline = perf_contract.make_report(
        repository=REPOSITORY,
        commit=commit,
        base_commit=None,  # 基线本身是参照点，无 base
        environment=env_fp,
        build={"type": env_fp.get("build_type"),
               "args": env_fp.get("cmake_args"),
               "compiler": env_fp.get("compiler")},
        parameters={"threads": threads, "dur": duration, "quick": quick,
                    "modules": MODULES},
        modules=modules,
        verdict=verdict,
    )
    baseline["micro_bench"] = micro  # 附加字段，不属于统一 Schema 必需键

    if output:
        path = Path(output)
    else:
        path = Path(BASELINE_DIR) / machine_slug / f"{ts}_{commit}.json"
    write_json_atomic(path, baseline)
    print(f"\nBaseline saved: {path}")
    return str(path)


def compare_baselines(path1, path2):
    """比较两份基线，判定复用 perf_contract；返回退出码。"""
    with open(path1) as f:
        b1 = json.load(f)
    with open(path2) as f:
        b2 = json.load(f)

    print("Comparing baselines:")
    print(f"  Old: {os.path.basename(path1)} (commit={b1.get('commit') or b1.get('git_commit', '?')})")
    print(f"  New: {os.path.basename(path2)} (commit={b2.get('commit') or b2.get('git_commit', '?')})")
    print()

    # 环境可比性：仅当两侧都有指纹时判定；旧格式基线提示未验证。
    comparable = True
    verdict = "PASS"
    env1, env2 = b1.get("environment"), b2.get("environment")
    if env1 and env2:
        env_result = perf_contract.compare_environment(env1, env2)
        if env_result.status != "PASS":
            comparable = False
            # 指纹不一致 → 聚合 verdict 必须为 NO_COMPARABLE_BASELINE，绝不转 PASS
            verdict = "NO_COMPARABLE_BASELINE"
            print(f"⚠️ 环境指纹不一致 ({env_result.detail.get('reason')}): "
                  f"{env_result.detail.get('keys')} — 结果标记 NO_COMPARABLE_BASELINE")
    else:
        print("ℹ️ 基线缺少环境指纹（旧格式）— 吞吐对比保留，但可比性未验证")

    issues = []
    print(f"{'Module':<12} {'Old ops/s':>10} {'New ops/s':>10} {'Delta%':>8} {'Verdict':>24}")
    print("-" * 74)

    for module in MODULES:
        old_mod = b1.get("modules", {}).get(module, {})
        new_mod = b2.get("modules", {}).get(module, {})

        if old_mod.get("error") or new_mod.get("error"):
            status = "SKIP"
            print(f"{module:<12} {'N/A':>10} {'N/A':>10} {'N/A':>8} {status:>24}")
            continue

        old_ops = old_mod.get("ops_per_sec", old_mod.get("ops_per_s", 0))
        new_ops = new_mod.get("ops_per_sec", new_mod.get("ops_per_s", 0))

        if not comparable:
            status = "NO_COMPARABLE_BASELINE"
            print(f"{module:<12} {old_ops:>10.0f} {new_ops:>10.0f} {'N/A':>8} {status:>24}")
            continue

        result = perf_contract.compare_module(
            old_ops, new_ops, module, gate_enabled=True)
        status = result.status
        if status == "FAIL":
            verdict = "FAIL"
            issues.append(f"{module}: {result.delta_pct:+.1f}%")
        elif status == "WARN" and verdict == "PASS":
            verdict = "WARN"
            issues.append(f"{module}: {result.delta_pct:+.1f}%")
        print(f"{module:<12} {old_ops:>10.0f} {new_ops:>10.0f} "
              f"{result.delta_pct:>+7.1f}% {status:>24}")

    # 锁延迟对比（兼容新旧字段）
    if comparable:
        print()
        print(f"{'Module':<12} {'Old avg_us':>10} {'New avg_us':>10} {'Delta%':>8}")
        print("-" * 48)
        for module in ["comutex", "corwmutex", "cocond"]:
            old_mod = b1.get("modules", {}).get(module, {})
            new_mod = b2.get("modules", {}).get(module, {})
            old_lat = (old_mod.get("lock_avg_us", 0) or
                       old_mod.get("wlock_avg_us", 0) or
                       old_mod.get("cond_avg_us", 0))
            new_lat = (new_mod.get("lock_avg_us", 0) or
                       new_mod.get("wlock_avg_us", 0) or
                       new_mod.get("cond_avg_us", 0))
            if isinstance(old_lat, (int, float)) and isinstance(new_lat, (int, float)) \
                    and old_lat > 0 and new_lat > 0:
                delta_lat = (new_lat - old_lat) / old_lat * 100
                marker = "⚠️" if delta_lat > 20 else "✅"
                print(f"{module:<12} {old_lat:>10.1f} {new_lat:>10.1f} {delta_lat:>+7.1f}% {marker}")

    # Micro-bench 对比（随 comparable 门控：环境不一致时不展示，与吞吐行一致）
    mb1, mb2 = b1.get("micro_bench"), b2.get("micro_bench")
    if (comparable and mb1 and mb2
            and "yield_roundtrip_avg_ns" in mb1
            and "yield_roundtrip_avg_ns" in mb2):
        old_sw = mb1["yield_roundtrip_avg_ns"]
        new_sw = mb2["yield_roundtrip_avg_ns"]
        if isinstance(old_sw, (int, float)) and old_sw > 0:
            delta_sw = (new_sw - old_sw) / old_sw * 100
            sw_marker = "⚠️" if delta_sw > 50 else "✅"
            print(f"\n{'co_switch':<12} {old_sw:>10.0f} {new_sw:>10.0f} {delta_sw:>+7.1f}% {sw_marker} (ns)")

    print()
    print(f"Overall verdict: {verdict}")
    if issues:
        print("Issues:")
        for issue in issues:
            print(f"  - {issue}")

    return 0 if verdict in ("PASS", "NO_COMPARABLE_BASELINE") else 1


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
    record.add_argument("--output", help="Baseline JSON output path (default: tests/baselines/)")
    record.add_argument("--environment-file", help="Load environment fingerprint from JSON file")

    compare = sub.add_parser("compare", help="Compare two baselines")
    compare.add_argument("baseline1", nargs="?", help="Old baseline path (default: penultimate)")
    compare.add_argument("baseline2", nargs="?", help="New baseline path (default: latest)")
    compare.add_argument("--latest-only", action="store_true", help="Compare latest two baselines")

    args = parser.parse_args()

    if args.command == "record":
        record_baseline(args.threads, args.dur, args.quick,
                        output=args.output, environment_file=args.environment_file)

    elif args.command == "compare":
        machine_slug = re.sub(r'[^\w\-]', '_', platform.node())
        if args.latest_only or (not args.baseline1 and not args.baseline2):
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
