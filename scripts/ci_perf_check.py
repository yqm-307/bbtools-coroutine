#!/usr/bin/env python3
"""
ci_perf_check.py — CI 门禁：快速性能回归检查（任务 4 统一 Schema 版）

用法:
  python3 scripts/ci_perf_check.py [--module MODULE] [--dur SECONDS] [--threads N]
  python3 scripts/ci_perf_check.py --module=comutex --threads=1 --dur=10 --no-baseline-compare
  python3 scripts/ci_perf_check.py --baseline <path> --gate-enabled

行为:
  - 运行 unified_stress，解析每个模块最后一个有效 FATIGUE_METRIC；
  - 拒绝 timeout / crash / zero ops / 缺字段（METRIC_INVALID）；
  - 输出统一 JSON 报告（perf_contract schema）与 Markdown 摘要；
  - 无基线或环境指纹不一致 → NO_COMPARABLE_BASELINE（exit 0）；
  - 门禁阈值 (ADR D6)：<10% PASS / 10%~20% WARN / >20% FAIL；
    --gate-enabled 默认关闭，>20% 退化仅报 WARN，不阻塞 PR；
  - 不在 PR Job 中隐式写基线（基线由 record_baseline.py 显式记录）。

退出码: PASS/NO_COMPARABLE_BASELINE → 0, WARN → 1, FAIL/METRIC_INVALID → 2。
"""
import argparse
import json
import os
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

MODULES = ["comutex", "corwmutex", "cocond", "chan", "copool", "coroutine"]
LATENCY_WARN = 20      # lock_avg_us +>20% → WARN
STALL_CYCLES = 2       # 连续 0 ops 周期 → 判定冻结
REPOSITORY = "bbtools-coroutine"

# 判定严重度：用于汇总整体 verdict（数值越大越严重）。
SEVERITY = {
    "FAIL": 5, "METRIC_INVALID": 4, "UNSTABLE": 3, "WARN": 2,
    "NO_COMPARABLE_BASELINE": 1, "PASS": 0,
}
EXIT_MAP = {"PASS": 0, "NO_COMPARABLE_BASELINE": 0, "WARN": 1,
            "FAIL": 2, "METRIC_INVALID": 2, "UNSTABLE": 2}


def get_machine_slug():
    return re.sub(r'[^\w\-]', '_', __import__('platform').node())


def get_git_commit():
    result = run_command(["git", "rev-parse", "--short", "HEAD"],
                         cwd=Path(PROJECT_DIR))
    if result.category != "pass":
        return "unknown"
    return result.stdout.strip()


def _read_build_info():
    """从 CMakeCache 读取 Build Type 与 CXX 标志（缺失时用默认值）。"""
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


def find_latest_baseline():
    """查找最新基线文件（兼容旧格式存储目录）。"""
    d = os.path.join(BASELINE_DIR, get_machine_slug())
    if not os.path.isdir(d):
        return None
    files = sorted([f for f in os.listdir(d) if f.endswith(".json")], reverse=True)
    return os.path.join(d, files[0]) if files else None


def run_benchmark(module, threads, duration):
    """运行 unified_stress 单模块并解析结果；失败返回 {"error": ...}。"""
    env = os.environ.copy()
    env["FATIGUE_INTERVAL"] = "10"

    cmd = [UNIFIED_STRESS, f"--module={module}", f"--threads={threads}",
           str(duration), "0", "0"]
    result = run_command(cmd, cwd=Path(BUILD_DIR), env=env,
                         timeout=duration + 45)

    if result.timed_out:
        return {"error": "timeout"}
    if result.returncode is not None and result.returncode < 0:
        return {"error": f"crash: rc={result.returncode}"}
    if result.category not in ("pass", "failure"):
        return {"error": f"unknown_exit: {result.category}"}
    if (result.stderr and ("assert" in result.stderr.lower() or
                           "Aborted" in result.stderr or
                           "Segmentation fault" in result.stderr)):
        return {"error": f"crash: {result.stderr[:200]}"}
    if result.returncode not in (0, None):
        return {"error": f"failure: rc={result.returncode}"}

    # 解析最后一个 FATIGUE_METRIC（name 字段精确匹配，防其他行内同名子串误匹配）
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
        return {"error": "no_metric"}

    normalized = perf_contract.normalize_module_metrics(metrics)
    if normalized is None:
        # 缺必需字段或类型非法（含 elapsed_s <= 0）：区分缺失与类型便于定位
        missing = [key for key in ("ops_total", "errors", "elapsed_s")
                   if key not in metrics]
        invalid = [key for key in ("ops_total", "errors", "elapsed_s")
                   if key in metrics
                   and (isinstance(metrics[key], bool)
                        or not isinstance(metrics[key], (int, float)))]
        if not missing and not invalid:
            invalid = ["elapsed_s"]  # elapsed_s 存在但 <= 0
        return {"error": "metric_invalid",
                "missing": missing or invalid}

    if normalized["elapsed_s"] < 10:
        return {"error": f"too_short: {normalized['elapsed_s']}s"}
    if normalized["ops_total"] == 0:
        return {"error": "zero_ops"}
    return normalized


def detect_stall(module, duration):
    """检测死锁冻结，返回判定原因字符串。

    返回 "stall"（超时未完成，或连续 STALL_CYCLES 个周期 0 增长）、
    "process_failure"（重跑进程崩溃/非零退出，不是冻结）、"no_data"
    （样本不足，无证据不下冻结结论）、"ok"（无冻结迹象）。
    崩溃与冻结都代表异常且最终同为 FAIL，但 reason 必须区分：
    把进程崩溃误报成 "STALL detected" 会掩盖真正的失败原因。
    """
    env = os.environ.copy()
    env["FATIGUE_INTERVAL"] = "5"  # 5s 采样
    cmd = [UNIFIED_STRESS, f"--module={module}", "--threads=2",
           str(duration), "0", "0"]
    result = run_command(cmd, cwd=Path(BUILD_DIR), env=env,
                         timeout=duration + 30)
    if result.timed_out:
        return "stall"  # 超时 = 进程未完成，视为冻结
    if result.returncode is not None and result.returncode != 0:
        return "process_failure"  # 被信号杀死（crash）或非零退出都不是冻结

    ops_history = []
    for line in result.stdout.split("\n"):
        if "FATIGUE_METRIC:" not in line:
            continue
        try:
            json_str = line.split("FATIGUE_METRIC:", 1)[1].strip()
            m = json.loads(json_str)
        except json.JSONDecodeError:
            continue
        if not isinstance(m, dict) or m.get("name") != module:
            continue
        try:
            ops_history.append(m["ops_total"])
        except (KeyError, TypeError):
            continue

    if len(ops_history) < 3:
        return "no_data"  # 数据不足，无证据不下冻结结论
    zeros = 0
    for i in range(1, len(ops_history)):
        if ops_history[i] == ops_history[i - 1]:
            zeros += 1
            if zeros >= STALL_CYCLES:
                return "stall"
        else:
            zeros = 0
    return "ok"


def check_latency(old_mod, new_mod):
    """检查锁延迟退化：lock/wlock/cond 任一平均延迟 +>20% → WARN。"""
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


def write_github_summary(results, verdict):
    """输出 GitHub Actions Step Summary（存在 GITHUB_STEP_SUMMARY 时）。

    overall 判定由调用方（main 聚合后的 verdict）传入，保证 summary 与
    终端/JSON 报告的判定一致，避免 results 行内无 verdict 键时恒打印 PASS。
    """
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not summary_path:
        return
    lines = ["## CI Performance Regression Check\n",
             "| Module | Old ops/s | New ops/s | Delta | Verdict |",
             "|--------|----------|----------|-------|---------|"]
    for r in results:
        mod = r["module"]
        status = r["status"]
        new_ops = r.get("new_ops", "N/A")
        old_ops = r.get("old_ops", "N/A") or "N/A"
        delta = r.get("delta_pct", 0)
        delta_str = f"{delta:+.1f}%" if isinstance(delta, (int, float)) else "N/A"
        emoji = {"PASS": "✅", "NO_COMPARABLE_BASELINE": "🆕",
                 "WARN": "⚠️", "FAIL": "❌",
                 "METRIC_INVALID": "❌"}.get(status, "❓")
        lines.append(f"| {mod} | {old_ops} | {new_ops} | {delta_str} | {emoji} {status} |")
    lines.append("")
    lines.append(f"**Overall**: {verdict}")
    with open(summary_path, "a") as f:
        f.write("\n".join(lines) + "\n")


def load_environment(args, modules, dur):
    """加载环境指纹：优先 --environment-file，否则本机采集。"""
    if args.environment_file:
        with open(args.environment_file) as f:
            env_fp = json.load(f)
        print(f"Environment loaded from: {args.environment_file}")
        return env_fp
    build_type, cmake_args = _read_build_info()
    return perf_contract.collect_environment_fingerprint(
        threads=args.threads, modules=modules, durations=[dur] * len(modules),
        build_type=build_type, cmake_args=cmake_args,
    )


def load_baseline(args):
    """加载基线：--baseline 显式路径优先，否则自动找最新；返回 (baseline, desc)。"""
    if args.no_baseline_compare or args.quick_smoke:
        return None, "skipped"
    path = args.baseline or find_latest_baseline()
    if not path:
        print("No baseline found — verdict: NO_COMPARABLE_BASELINE")
        return None, "none"
    if not os.path.exists(path):
        print(f"ERROR: baseline not found: {path}")
        sys.exit(2)
    with open(path) as f:
        baseline = json.load(f)
    print(f"Baseline: {os.path.basename(path)}")
    return baseline, path


def main():
    parser = argparse.ArgumentParser(description="CI performance regression check")
    parser.add_argument("--module", help="Single module to check (default: all 6)")
    parser.add_argument("--dur", type=int, default=45, help="Duration per module (seconds)")
    parser.add_argument("--threads", type=int, default=2, help="Processer threads")
    parser.add_argument("--baseline", help="Explicit baseline JSON path")
    parser.add_argument("--output", help="Unified report JSON path (default: tests/ci-reports/)")
    parser.add_argument("--environment-file", help="Load environment fingerprint from JSON file")
    parser.add_argument("--gate-enabled", action="store_true",
                        help="Enable FAIL gate (default off: >20%% drop reports WARN only)")
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

    env_fp = load_environment(args, modules, dur)
    baseline, baseline_desc = load_baseline(args)

    # 环境可比性：指纹不一致时整体降级为 NO_COMPARABLE_BASELINE
    comparable = True
    if baseline is not None:
        env_result = perf_contract.compare_environment(
            baseline.get("environment"), env_fp)
        if env_result.status != "PASS":
            comparable = False
            print(f"⚠️ Environment mismatch ({env_result.detail.get('reason')}), "
                  f"skipping comparison: {env_result.detail.get('keys')}")

    results = []
    for module in modules:
        print(f"\n[{module}] running {dur}s...", end=" ", flush=True)
        t0 = time.time()
        new_mod = run_benchmark(module, args.threads, dur)
        elapsed = time.time() - t0

        entry = {"module": module, "old_ops": None, "delta_pct": None}
        if new_mod.get("error"):
            status = "METRIC_INVALID" if new_mod["error"] == "metric_invalid" else "FAIL"
            entry.update({"status": status, "error": new_mod["error"]})
            if "missing" in new_mod:
                entry["missing"] = new_mod["missing"]
            print(f"❌ {new_mod['error']} ({elapsed:.1f}s)")
            results.append(entry)
            continue

        new_ops = new_mod["ops_per_sec"]
        print(f"{new_ops:.0f} ops/s ({elapsed:.1f}s)")
        entry.update({"new_ops": new_ops, **new_mod})

        if not comparable or baseline is None:
            result = perf_contract.CompareResult(
                status="NO_COMPARABLE_BASELINE", module=module,
                new_ops=new_ops,
                detail={"reason": "no_baseline" if baseline is None
                        else "environment_mismatch"})
        else:
            old_mod = baseline.get("modules", {}).get(module)
            if old_mod is None or old_mod.get("error"):
                result = perf_contract.CompareResult(
                    status="NO_COMPARABLE_BASELINE", module=module,
                    detail={"reason": "baseline_module_missing"})
            else:
                result = perf_contract.compare_module(
                    old_mod.get("ops_per_sec", 0), new_ops, module,
                    gate_enabled=args.gate_enabled)

        status = result.status
        entry.update({"status": status, "old_ops": result.old_ops or None})
        if status in ("PASS", "WARN", "FAIL"):
            entry["delta_pct"] = result.delta_pct

        # 延迟检查
        latency_warn = False
        if baseline is not None and comparable:
            old_mod = baseline.get("modules", {}).get(module)
            old_lat = old_mod.get("lock_avg_us", 0) if old_mod else 0
            if isinstance(old_lat, (int, float)) and old_lat > 0:
                latency_warn, latency_delta = check_latency(old_mod, new_mod)
                if latency_warn:
                    print(f"  ⚠️ lock_avg_us +{latency_delta:.1f}%")
                    status = "WARN"
                    entry["status"] = "WARN"

        # 冻结检测（仅长时间测试且出现错误计数）
        if dur >= 30 and new_mod.get("errors", 0) > 0:
            print(f"  [stall check] errors={new_mod['errors']}, checking stall...")
            stall_reason = detect_stall(module, min(dur, 30))
            if stall_reason in ("stall", "process_failure"):
                # 冻结与进程失败都是 FAIL，但 reason 区分，避免崩溃误报为冻结
                entry["reason"] = stall_reason
                if status != "FAIL":
                    status = "FAIL"
                    entry["status"] = "FAIL"
                    label = ("STALL detected" if stall_reason == "stall"
                             else "stall check process failed")
                    print(f"  ❌ {label}")
            elif stall_reason == "no_data":
                print("  ⚠️ stall check insufficient samples, keeping status")

        results.append(entry)

    verdict = max(results, key=lambda r: SEVERITY[r["status"]])["status"]

    # 统一 JSON + Markdown 输出（报告非基线：不写 tests/baselines/）
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    if args.output:
        json_path = Path(args.output)
    else:
        json_path = Path(PROJECT_DIR) / "tests" / "ci-reports" / f"perf_{ts}.json"
    report = perf_contract.make_report(
        repository=REPOSITORY,
        commit=get_git_commit(),
        base_commit=baseline.get("commit") if baseline else None,
        environment=env_fp,
        build={"type": env_fp.get("build_type"), "args": env_fp.get("cmake_args"),
               "compiler": env_fp.get("compiler")},
        parameters={"threads": args.threads, "dur": dur,
                    "module": args.module, "gate_enabled": args.gate_enabled,
                    "baseline": baseline_desc},
        modules={r["module"]: {k: v for k, v in r.items() if k != "module"}
                 for r in results},
        verdict=verdict,
    )
    check = perf_contract.validate_report(report)
    if check.status != "PASS":
        print(f"ERROR: report invalid: {check.missing}")
        sys.exit(2)
    write_json_atomic(json_path, report)
    perf_contract.write_markdown_report(json_path.with_suffix(".md"), report)
    print(f"\nReport: {json_path}")
    print(f"Markdown: {json_path.with_suffix('.md')}")

    write_github_summary(results, verdict)

    # Terminal summary
    print(f"\n{'Module':<12} {'Old ops/s':>10} {'New ops/s':>10} {'Delta':>8} {'Verdict':>24}")
    print("-" * 70)
    for r in results:
        emoji = {"PASS": "✅", "NO_COMPARABLE_BASELINE": "🆕",
                 "WARN": "⚠️", "FAIL": "❌", "METRIC_INVALID": "❌"}.get(r["status"], "❓")
        delta_str = (f"{r['delta_pct']:+.1f}%" if isinstance(r.get("delta_pct"), (int, float))
                     else "N/A")
        print(f"{r['module']:<12} {str(r.get('old_ops') or 'N/A'):>10} "
              f"{str(r.get('new_ops') or 'N/A'):>10} {delta_str:>8} {emoji} {r['status']}")

    print(f"\nOverall: {verdict}")
    sys.exit(EXIT_MAP.get(verdict, 2))


if __name__ == "__main__":
    main()
