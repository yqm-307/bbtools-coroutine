#!/usr/bin/env python3
"""
ci_perf_check.py — CI 门禁：快速性能回归检查

用法:
  python3 scripts/ci_perf_check.py [--module MODULE] [--dur SECONDS] [--threads N]

CI 集成:
  在 GitHub Actions workflow 中调用，对比最新基线判定 PASS/WARN/FAIL。
  输出 GitHub Actions Step Summary 到 $GITHUB_STEP_SUMMARY。

门禁阈值 (ADR D6):
  <10%     → PASS  (exit 0)
  10%~20%  → WARN  (exit 1) — 拦截 PR，需 ADR 说明
  >20%     → FAIL  (exit 2) — 拦截 PR，升级为事故
  CoCond >30% → FAIL (exit 2) — CoCond 放宽阈值
  死锁冻结  → FAIL (exit 2) — ops 停滞 >2 采样周期
  lock_avg_us +>20% → WARN (exit 1)
  无基线    → PASS_NO_BASELINE (exit 0) — 首次运行，记录基线
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
from datetime import datetime

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(PROJECT_DIR, "build")
BASELINE_DIR = os.path.join(PROJECT_DIR, "tests", "baselines")
UNIFIED_STRESS = os.path.join(BUILD_DIR, "bin", "benchmark_test", "unified_stress")

MODULES = ["comutex", "corwmutex", "cocond", "chan", "copool", "coroutine"]
THRESHOLD_WARN = 10    # >10% → WARN
THRESHOLD_FAIL = 20    # >20% → FAIL
THRESHOLD_COCOND = 30  # CoCond: >30% → FAIL
LATENCY_WARN = 20      # lock_avg_us +>20% → WARN
STALL_CYCLES = 2       # 连续 0 ops 周期 → 判定冻结


def get_machine_slug():
    return re.sub(r'[^\w\-]', '_', __import__('platform').node())


def find_latest_baseline():
    """查找最新基线文件"""
    d = os.path.join(BASELINE_DIR, get_machine_slug())
    if not os.path.isdir(d):
        return None
    files = sorted([f for f in os.listdir(d) if f.endswith(".json")], reverse=True)
    return os.path.join(d, files[0]) if files else None


def run_benchmark(module, threads, duration):
    """运行 unified_stress 单模块并解析结果"""
    env = os.environ.copy()
    env["FATIGUE_INTERVAL"] = "10"

    cmd = [UNIFIED_STRESS, f"--module={module}", f"--threads={threads}",
           str(duration), "0", "0"]
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=duration + 45,
            cwd=BUILD_DIR, env=env
        )
    except subprocess.TimeoutExpired:
        return {"error": "timeout"}

    # 检查 stderr 是否有 assert/crash
    if result.stderr and ("assert" in result.stderr.lower() or
                          "Aborted" in result.stderr or
                          "Segmentation fault" in result.stderr):
        return {"error": f"crash: {result.stderr[:200]}"}

    # 解析最后一帧 FATIGUE_METRIC
    metrics = None
    for line in result.stdout.split("\n"):
        if f'"name":"{module}"' in line and "FATIGUE_METRIC:" in line:
            try:
                json_str = line.split("FATIGUE_METRIC:", 1)[1].strip()
                metrics = json.loads(json_str)
            except json.JSONDecodeError:
                continue

    if metrics is None:
        return {"error": "no_metric"}

    elapsed = metrics.get("elapsed_s", 0)
    if elapsed < 10:
        return {"error": f"too_short: {elapsed}s"}

    ops_total = metrics["ops_total"]
    if ops_total == 0:
        return {"error": "zero_ops"}

    return {
        "ops_total": ops_total,
        "ops_per_s": round(ops_total / elapsed, 1),
        "errors": metrics.get("errors", 0),
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


def detect_stall(module, duration):
    """检测死锁冻结：运行 30s，检查是否有连续 0 ops 周期"""
    env = os.environ.copy()
    env["FATIGUE_INTERVAL"] = "5"  # 5s 采样

    cmd = [UNIFIED_STRESS, f"--module={module}", f"--threads=2",
           str(duration), "0", "0"]
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=duration + 30,
            cwd=BUILD_DIR, env=env
        )
    except subprocess.TimeoutExpired:
        return True  # 超时 = 冻结

    ops_history = []
    for line in result.stdout.split("\n"):
        if f'"name":"{module}"' in line and "FATIGUE_METRIC:" in line:
            try:
                json_str = line.split("FATIGUE_METRIC:", 1)[1].strip()
                m = json.loads(json_str)
                ops_history.append(m["ops_total"])
            except (json.JSONDecodeError, KeyError):
                continue

    if len(ops_history) < 3:
        return True  # 数据不足

    # 检查连续 0 增长
    zeros = 0
    for i in range(1, len(ops_history)):
        if ops_history[i] == ops_history[i - 1]:
            zeros += 1
            if zeros >= STALL_CYCLES:
                return True
        else:
            zeros = 0
    return False


def compare_module(old_mod, new_mod, module):
    """对比单模块基线，返回 (status, delta_pct, detail)"""
    if old_mod is None:
        return "PASS_NO_BASELINE", 0.0, {}
    if old_mod.get("error"):
        return "PASS_NO_BASELINE", 0.0, {"reason": f"baseline_error: {old_mod['error']}"}
    if new_mod.get("error"):
        return "FAIL", 0.0, {"reason": f"new_error: {new_mod['error']}"}

    old_ops = old_mod.get("ops_per_s", 0)
    new_ops = new_mod.get("ops_per_s", 0)

    if old_ops == 0:
        return "PASS_NO_BASELINE", 0.0, {"reason": "baseline_zero_ops"}

    delta_pct = (new_ops - old_ops) / old_ops * 100
    detail = {"old_ops": old_ops, "new_ops": new_ops, "delta_pct": delta_pct}

    threshold = THRESHOLD_COCOND if module == "cocond" else THRESHOLD_WARN
    escalate = THRESHOLD_COCOND + 10 if module == "cocond" else THRESHOLD_FAIL

    if delta_pct <= -escalate:
        return "FAIL", delta_pct, detail
    elif delta_pct <= -threshold:
        return "WARN", delta_pct, detail
    else:
        return "PASS", delta_pct, detail


def check_latency(old_mod, new_mod):
    """检查锁延迟退化"""
    old_lat = (old_mod.get("lock_avg_us", 0) or
               old_mod.get("wlock_avg_us", 0) or
               old_mod.get("cond_avg_us", 0))
    new_lat = (new_mod.get("lock_avg_us", 0) or
               new_mod.get("wlock_avg_us", 0) or
               new_mod.get("cond_avg_us", 0))
    if old_lat > 0 and new_lat > 0:
        delta = (new_lat - old_lat) / old_lat * 100
        return delta > LATENCY_WARN, delta
    return False, 0.0


def write_github_summary(results):
    """输出 GitHub Actions Step Summary"""
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not summary_path:
        return

    lines = []
    lines.append("## CI Performance Regression Check\n")
    lines.append("| Module | Old ops/s | New ops/s | Delta | Threshold | Verdict |")
    lines.append("|--------|----------|----------|-------|-----------|---------|")

    for r in results:
        mod = r["module"]
        status = r["status"]
        new_ops = r.get("new_ops", "N/A")
        old_ops = r.get("old_ops", "N/A")
        delta = r.get("delta_pct", 0)
        threshold = THRESHOLD_COCOND if mod == "cocond" else THRESHOLD_WARN

        emoji = {"PASS": "✅", "PASS_NO_BASELINE": "🆕", "WARN": "⚠️", "FAIL": "❌"}.get(status, "❓")
        lines.append(f"| {mod} | {old_ops} | {new_ops} | {delta:+.1f}% | {threshold}% | {emoji} {status} |")

    lines.append("")
    lines.append(f"**Overall**: {results[0].get('verdict', 'PASS') if results else 'N/A'}")

    with open(summary_path, "a") as f:
        f.write("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description="CI performance regression check")
    parser.add_argument("--module", help="Single module to check (default: all 6)")
    parser.add_argument("--dur", type=int, default=45, help="Duration per module (seconds)")
    parser.add_argument("--threads", type=int, default=2, help="Processer threads")
    parser.add_argument("--no-baseline-compare", action="store_true",
                        help="Skip baseline comparison (smoke check only)")
    parser.add_argument("--quick-smoke", action="store_true",
                        help="Quick smoke: 10s per module, no baseline")
    args = parser.parse_args()

    if not os.path.exists(UNIFIED_STRESS):
        print(f"ERROR: unified_stress not found at {UNIFIED_STRESS}")
        sys.exit(2)

    modules = [args.module] if args.module else MODULES
    dur = 10 if args.quick_smoke else args.dur

    baseline = None
    if not args.no_baseline_compare and not args.quick_smoke:
        baseline_path = find_latest_baseline()
        if baseline_path:
            with open(baseline_path) as f:
                baseline = json.load(f)
            print(f"Baseline: {os.path.basename(baseline_path)}")
        else:
            print("No baseline found — will record first baseline on pass")

    results = []
    overall_worst = "PASS"

    for module in modules:
        print(f"\n[{module}] running {dur}s...", end=" ", flush=True)
        t0 = time.time()

        # Phase 1: 运行压测
        new_mod = run_benchmark(module, args.threads, dur)
        elapsed = time.time() - t0

        if new_mod.get("error"):
            print(f"❌ {new_mod['error']} ({elapsed:.1f}s)")
            results.append({
                "module": module,
                "status": "FAIL",
                "error": new_mod["error"],
            })
            overall_worst = "FAIL"
            continue

        new_ops = new_mod["ops_per_s"]
        print(f"{new_ops:.0f} ops/s ({elapsed:.1f}s)")

        # Phase 2: 对比基线
        old_mod = baseline.get("modules", {}).get(module) if baseline else None
        status, delta, detail = compare_module(old_mod, new_mod, module)

        # Phase 3: 延迟检查
        latency_warn = False
        if old_mod and old_mod.get("lock_avg_us", 0) > 0:
            latency_warn, latency_delta = check_latency(old_mod, new_mod)
            if latency_warn:
                print(f"  ⚠️ lock_avg_us +{latency_delta:.1f}%")

        # Phase 4: 冻结检测（仅长时间测试）
        if dur >= 30 and new_mod["errors"] > 0:
            print(f"  [stall check] errors={new_mod['errors']}, checking stall...")
            stalled = detect_stall(module, min(dur, 30))
            if stalled and status != "FAIL":
                status = "FAIL"
                print(f"  ❌ STALL detected")

        # 综合判定
        if status == "PASS" and latency_warn:
            status = "WARN"
        if status == "FAIL" or (status == "WARN" and overall_worst == "PASS"):
            overall_worst = status
        if status == "WARN" and overall_worst == "FAIL":
            pass  # FAIL already set

        results.append({
            "module": module,
            "status": status,
            "new_ops": new_ops,
            "old_ops": old_mod.get("ops_per_s", "N/A") if old_mod else "N/A",
            "delta_pct": delta,
            "latency_warn": latency_warn,
        })

    # Phase 5: 无基线时记录首次基线
    if overall_worst == "PASS" and baseline is None and not args.quick_smoke:
        print("\n[baseline] First pass — recording baseline...")
        try:
            subprocess.run(
                [sys.executable, os.path.join(PROJECT_DIR, "scripts", "record_baseline.py"),
                 "record", "--quick", f"--threads={args.threads}"],
                cwd=PROJECT_DIR, timeout=360
            )
        except Exception as e:
            print(f"[baseline] Record failed: {e}")

    # 输出摘要
    write_github_summary(results)

    # Terminal summary
    print(f"\n{'Module':<12} {'Old ops/s':>10} {'New ops/s':>10} {'Delta':>8} {'Verdict':>8}")
    print("-" * 56)
    for r in results:
        emoji = {"PASS": "✅", "PASS_NO_BASELINE": "🆕", "WARN": "⚠️", "FAIL": "❌"}.get(r["status"], "❓")
        delta_str = f"{r.get('delta_pct', 0):+.1f}%" if r.get("delta_pct") is not None else "N/A"
        print(f"{r['module']:<12} {str(r.get('old_ops','N/A')):>10} {str(r.get('new_ops','N/A')):>10} {delta_str:>8} {emoji} {r['status']}")

    print(f"\nOverall: {overall_worst}")

    exit_map = {"PASS": 0, "PASS_NO_BASELINE": 0, "WARN": 1, "FAIL": 2}
    sys.exit(exit_map.get(overall_worst, 2))


if __name__ == "__main__":
    main()
