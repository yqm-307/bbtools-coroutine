#!/usr/bin/env python3
"""
run_fatigue.py — 疲劳测试运行器 & 报告生成器

用法：
    # 运行所有疲劳测试 1 小时
    python3 scripts/run_fatigue.py --duration 3600

    # 只跑 comutex 和 coroutine，30 分钟
    python3 scripts/run_fatigue.py --duration 1800 --filter comutex,coroutine

    # 对比最近两次报告
    python3 scripts/run_fatigue.py --compare

    # 查看最近一次报告
    python3 scripts/run_fatigue.py --show-latest

输出：
    tests/reports/<YYYY-MM-DD_HH-MM-SS>/
    ├── summary.json         # 运行总览
    ├── <module>.json        # 每个模块的详细数据
    ├── summary.md           # Markdown 可读报告
    └── raw/                 # 原始 stdout 日志
"""

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from datetime import datetime, timezone, timedelta
from pathlib import Path
from typing import Dict, List, Optional
from collections import defaultdict

PROJECT_ROOT = Path(__file__).resolve().parent.parent
REPORTS_DIR = PROJECT_ROOT / "tests" / "reports"
BUILD_DIR = PROJECT_ROOT / "build_cov"
BIN_DIR = BUILD_DIR / "bin" / "benchmark_test"
LATEST_LINK = REPORTS_DIR / "latest"

# 所有可用的疲劳测试模块
ALL_MODULES = {
    "coroutine":    "fatigue_coroutine",
    "chan":         "fatigue_chan",
    "comutex":      "fatigue_comutex",
    "corwmutex":    "fatigue_corwmutex",
    "cocond":       "fatigue_cocond",
    "copool":       "fatigue_copool",
    "std_uniquelock": "fatigue_std_uniquelock",
}

TZ = timezone(timedelta(hours=8))  # Asia/Shanghai


def find_binaries(modules: List[str]) -> Dict[str, Path]:
    """查找编译好的疲劳测试二进制文件"""
    result = {}
    for mod in modules:
        name = ALL_MODULES.get(mod, mod)
        path = BIN_DIR / name
        if path.exists():
            result[mod] = path
        else:
            print(f"[WARN] 二进制不存在，跳过: {path}")
    return result


def run_test(binary: Path, duration_s: int) -> dict:
    """运行单个疲劳测试，收集 FATIGUE_METRIC 行。返回运行结果"""
    start_wall = time.time()
    metrics_lines: List[str] = []
    stdout_lines: List[str] = []

    proc = subprocess.Popen(
        [str(binary)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        preexec_fn=os.setsid,
    )

    try:
        proc.wait(timeout=duration_s)
    except subprocess.TimeoutExpired:
        # 超时，优雅终止
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
            proc.wait(timeout=5)
        except (subprocess.TimeoutExpired, ProcessLookupError):
            try:
                os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            except (ProcessLookupError, OSError):
                pass
    except Exception as e:
        print(f"[ERROR] 进程异常: {e}")

    # 读取所有输出
    if proc.stdout:
        for line in proc.stdout:
            line = line.rstrip("\n")
            stdout_lines.append(line)
            if line.startswith("FATIGUE_METRIC:"):
                metrics_lines.append(line)

    end_wall = time.time()
    return {
        "exit_code": proc.returncode,
        "wall_duration_s": round(end_wall - start_wall, 1),
        "metrics_lines": metrics_lines,
        "stdout_lines": stdout_lines,
    }


def parse_metrics(metrics_lines: List[str]) -> List[dict]:
    """解析 FATIGUE_METRIC: 行"""
    result = []
    for line in metrics_lines:
        try:
            json_str = line[len("FATIGUE_METRIC:"):]
            data = json.loads(json_str)
            result.append(data)
        except (json.JSONDecodeError, KeyError):
            continue
    return result


def compute_summary(module_name: str, metrics: List[dict], wall_s: float) -> dict:
    """从 metrics 时间序列计算汇总统计"""
    if not metrics:
        return {
            "module": module_name,
            "wall_duration_s": wall_s,
            "samples": 0,
            "error": "no metrics collected",
        }

    last = metrics[-1]
    first = metrics[0] if len(metrics) > 1 else {"elapsed_s": 0, **{k: 0 for k in last}}

    def rate(field: str) -> float:
        """计算速率 (ops/sec)，单采样点用绝对值/时长"""
        if len(metrics) == 1:
            elapsed = last.get("elapsed_s", wall_s)
            return round(last.get(field, 0) / elapsed, 1) if elapsed > 0 else 0.0
        delta_val = last.get(field, 0) - first.get(field, 0)
        delta_t = last.get("elapsed_s", 0) - first.get("elapsed_s", 0)
        if delta_t <= 0:
            return 0.0
        return round(delta_val / delta_t, 1)

    # 提取所有采样的某个字段
    def field_values(field: str) -> List:
        return [m.get(field, 0) for m in metrics if field in m]

    lock_avg_vals = field_values("lock_avg_us")
    cond_avg_vals = field_values("cond_avg_us")

    summary = {
        "module": module_name,
        "wall_duration_s": wall_s,
        "test_elapsed_s": last.get("elapsed_s", 0),
        "samples": len(metrics),
        "exit_code": 0,  # will be overwritten

        # 最终值
        "ops_total": last.get("ops_total", 0),
        "errors": last.get("errors", 0),

        # 速率
        "ops_per_sec": rate("ops_total"),
        "lock_ops_per_sec": rate("lock_ops"),
        "trylock_success_per_sec": rate("trylock_success"),
        "trylock_timeout_per_sec": rate("trylock_timeout"),

        # 锁延迟 (us) — 取最后一次采样的平均值
        "lock_avg_us": lock_avg_vals[-1] if lock_avg_vals else 0,
        "lock_avg_us_max": max(lock_avg_vals) if lock_avg_vals else 0,
        "lock_avg_us_min": min(lock_avg_vals) if lock_avg_vals else 0,

        # 最终计数器
        "lock_ops": last.get("lock_ops", 0),
        "trylock_success": last.get("trylock_success", 0),
        "trylock_timeout": last.get("trylock_timeout", 0),
        "rlock_ops": last.get("rlock_ops", 0),
        "wlock_ops": last.get("wlock_ops", 0),
        "cond_waits": last.get("cond_waits", 0),
        "cond_signals": last.get("cond_signals", 0),
        "cond_timeouts": last.get("cond_timeouts", 0),
        "cond_avg_us": cond_avg_vals[-1] if cond_avg_vals else 0,
        "chan_reads": last.get("chan_reads", 0),
        "chan_writes": last.get("chan_writes", 0),
        "pool_tasks": last.get("pool_tasks", 0),
        "pool_completed": last.get("pool_completed", 0),
        "pool_dropped": last.get("pool_dropped", 0),
    }

    return summary


def generate_markdown(run_id: str, summaries: List[dict],
                      total_duration_s: float) -> str:
    """生成 Markdown 可读报告"""
    now = datetime.now(TZ).strftime("%Y-%m-%d %H:%M:%S")
    lines = [
        f"# 疲劳测试报告",
        f"",
        f"**运行 ID**: `{run_id}`  ",
        f"**时间**: {now}  ",
        f"**计划时长**: {int(total_duration_s)}s ({total_duration_s/60:.0f}min)  ",
        f"**模块数**: {len(summaries)}",
        f"",
        f"---",
        f"",
    ]

    for s in summaries:
        status = "✅" if s.get("exit_code", 0) in (0, -15, None) and s.get("errors", 0) == 0 else "❌"
        m = s["module"]
        lines.append(f"## {status} {m}")
        lines.append("")
        lines.append(f"| 指标 | 值 |")
        lines.append(f"|------|----|")
        lines.append(f"| 实际运行时长 | {s.get('wall_duration_s', 0):.0f}s |")
        lines.append(f"| 采样次数 | {s.get('samples', 0)} |")
        lines.append(f"| ops_total | {s.get('ops_total', 0):,} |")
        lines.append(f"| ops/sec | {s.get('ops_per_sec', 0):,} |")
        lines.append(f"| errors | {s.get('errors', 0)} |")

        if s.get("lock_ops", 0) > 0:
            lines.append(f"| lock_ops | {s['lock_ops']:,} |")
            lines.append(f"| lock_ops/sec | {s.get('lock_ops_per_sec', 0):,} |")
            lines.append(f"| lock_avg_us | {s.get('lock_avg_us', 0):.1f} |")
            lines.append(f"| lock_avg_us (max) | {s.get('lock_avg_us_max', 0):.1f} |")

        if s.get("trylock_success", 0) > 0 or s.get("trylock_timeout", 0) > 0:
            total_tl = s["trylock_success"] + s["trylock_timeout"]
            timeout_rate = (s["trylock_timeout"] / total_tl * 100) if total_tl > 0 else 0
            lines.append(f"| trylock_success | {s['trylock_success']:,} |")
            lines.append(f"| trylock_timeout | {s['trylock_timeout']:,} |")
            lines.append(f"| trylock 超时率 | {timeout_rate:.1f}% |")

        if s.get("rlock_ops", 0) > 0 or s.get("wlock_ops", 0) > 0:
            lines.append(f"| rlock_ops | {s['rlock_ops']:,} |")
            lines.append(f"| wlock_ops | {s['wlock_ops']:,} |")

        if s.get("cond_waits", 0) > 0:
            lines.append(f"| cond_waits | {s['cond_waits']:,} |")
            lines.append(f"| cond_signals | {s['cond_signals']:,} |")
            lines.append(f"| cond_timeouts | {s['cond_timeouts']:,} |")

        if s.get("chan_reads", 0) > 0:
            lines.append(f"| chan_reads | {s['chan_reads']:,} |")
            lines.append(f"| chan_writes | {s['chan_writes']:,} |")

        if s.get("pool_tasks", 0) > 0:
            lines.append(f"| pool_tasks | {s['pool_tasks']:,} |")
            lines.append(f"| pool_completed | {s['pool_completed']:,} |")
            lines.append(f"| pool_dropped | {s['pool_dropped']:,} |")

        lines.append("")

    return "\n".join(lines)


def save_report(run_id: str, summaries: List[dict],
                raw_outputs: Dict[str, List[str]],
                all_metrics: Dict[str, List[dict]],
                total_duration_s: float) -> Path:
    """保存报告到 tests/reports/<run_id>/"""
    run_dir = REPORTS_DIR / run_id
    run_dir.mkdir(parents=True, exist_ok=True)

    # 总览 JSON
    overview = {
        "run_id": run_id,
        "timestamp": datetime.now(TZ).isoformat(),
        "duration_s": total_duration_s,
        "modules": summaries,
    }
    (run_dir / "summary.json").write_text(
        json.dumps(overview, indent=2, ensure_ascii=False))

    # 每模块详细 JSON（完整指标时间序列）
    for mod, metrics in all_metrics.items():
        (run_dir / f"{mod}.json").write_text(
            json.dumps(metrics, indent=2, ensure_ascii=False))

    # Markdown 报告
    md = generate_markdown(run_id, summaries, total_duration_s)
    (run_dir / "summary.md").write_text(md)

    # 原始日志
    raw_dir = run_dir / "raw"
    raw_dir.mkdir(exist_ok=True)
    for mod, lines in raw_outputs.items():
        (raw_dir / f"{mod}.log").write_text("\n".join(lines))

    # 更新 latest 链接
    if LATEST_LINK.is_symlink() or LATEST_LINK.exists():
        LATEST_LINK.unlink()
    LATEST_LINK.symlink_to(run_dir.name, target_is_directory=True)

    return run_dir


def load_report(run_id: Optional[str] = None) -> Optional[dict]:
    """加载指定或最近的报告"""
    if run_id:
        path = REPORTS_DIR / run_id / "summary.json"
    elif LATEST_LINK.exists():
        path = LATEST_LINK / "summary.json"
    else:
        # 找最近的
        dirs = sorted([d for d in REPORTS_DIR.iterdir()
                       if d.is_dir() and d.name != "latest"], reverse=True)
        if not dirs:
            return None
        path = dirs[0] / "summary.json"

    if path.exists():
        return json.loads(path.read_text())
    return None


def compare_reports(report_a: dict, report_b: dict):
    """对比两次报告，打印差异"""
    mods_a = {m["module"]: m for m in report_a.get("modules", [])}
    mods_b = {m["module"]: m for m in report_b.get("modules", [])}

    print(f"\n{'='*70}")
    print(f"对比报告:")
    print(f"  A: {report_a.get('run_id', '?')}  ({report_a.get('timestamp', '?')})")
    print(f"  B: {report_b.get('run_id', '?')}  ({report_b.get('timestamp', '?')})")
    print(f"{'='*70}")

    all_modules = sorted(set(mods_a.keys()) | set(mods_b.keys()))
    for mod in all_modules:
        a = mods_a.get(mod, {})
        b = mods_b.get(mod, {})
        print(f"\n## {mod}")

        # 关键指标对比
        keys = [
            ("ops_per_sec", "ops/sec"),
            ("lock_ops_per_sec", "lock ops/sec"),
            ("lock_avg_us", "lock avg(us)"),
            ("trylock_timeout_per_sec", "trylock timeout/sec"),
        ]
        for key, label in keys:
            va = a.get(key, 0)
            vb = b.get(key, 0)
            if va == 0 and vb == 0:
                continue
            delta = vb - va
            pct = (delta / va * 100) if va != 0 else 0
            sign = "+" if delta > 0 else ""
            print(f"  {label:25s}: {va:>12,.1f} → {vb:>12,.1f}  ({sign}{delta:,.1f}, {sign}{pct:.1f}%)")

    print()


def show_latest():
    """打印最近一次报告的摘要"""
    report = load_report()
    if not report:
        print("没有找到报告。先运行一次疲劳测试。")
        return

    print(f"\n{'='*70}")
    print(f"最近报告: {report['run_id']}")
    print(f"时间: {report.get('timestamp', '?')}")
    print(f"时长: {report.get('duration_s', 0):.0f}s")
    print(f"{'='*70}")

    for m in report.get("modules", []):
        status = "✅" if m.get("errors", 0) == 0 else "❌"
        print(f"\n{status} {m['module']}")
        print(f"  ops: {m.get('ops_total', 0):,}  ({m.get('ops_per_sec', 0):,}/s)")
        if m.get('lock_ops', 0) > 0:
            print(f"  lock: {m.get('lock_ops', 0):,}  ({m.get('lock_ops_per_sec', 0):,}/s)  "
                  f"avg={m.get('lock_avg_us', 0):.1f}us")
        if m.get('trylock_timeout', 0) > 0:
            total = m['trylock_success'] + m['trylock_timeout']
            rate = m['trylock_timeout'] / total * 100 if total > 0 else 0
            print(f"  trylock: success={m['trylock_success']:,} timeout={m['trylock_timeout']:,} ({rate:.1f}%)")
        if m.get('rlock_ops', 0) > 0:
            print(f"  rwlock: r={m['rlock_ops']:,} w={m['wlock_ops']:,}")

    markdown_path = REPORTS_DIR / report['run_id'] / "summary.md"
    if markdown_path.exists():
        print(f"\n📄 完整报告: {markdown_path}")


def main():
    parser = argparse.ArgumentParser(description="疲劳测试运行器")
    parser.add_argument("--duration", type=int, default=3600,
                        help="运行时长(秒), 默认 3600 (1小时)")
    parser.add_argument("--filter", type=str, default="",
                        help="逗号分隔的模块列表, 默认全部")
    parser.add_argument("--compare", action="store_true",
                        help="对比最近两次报告")
    parser.add_argument("--show-latest", action="store_true",
                        help="显示最近一次报告摘要")
    args = parser.parse_args()

    if args.compare:
        reports = sorted(
            [d for d in REPORTS_DIR.iterdir()
             if d.is_dir() and d.name != "latest" and (d / "summary.json").exists()],
            reverse=True)
        if len(reports) >= 2:
            a = json.loads((reports[1] / "summary.json").read_text())
            b = json.loads((reports[0] / "summary.json").read_text())
            compare_reports(a, b)
        else:
            print("需要至少两次报告才能对比。先运行疲劳测试。")
        return

    if args.show_latest:
        show_latest()
        return

    # ── 运行模式 ──
    modules = [m.strip() for m in args.filter.split(",") if m.strip()] if args.filter else list(ALL_MODULES.keys())
    binaries = find_binaries(modules)
    if not binaries:
        print("[ERROR] 没有找到任何可用的测试二进制。请先编译。")
        sys.exit(1)

    run_id = datetime.now(TZ).strftime("%Y-%m-%d_%H-%M-%S")
    print(f"🚀 疲劳测试开始")
    print(f"   Run ID:  {run_id}")
    print(f"   时长:    {args.duration}s ({args.duration/60:.0f}min)")
    print(f"   模块:    {', '.join(binaries.keys())}")
    print(f"   二进制:  {BIN_DIR}")
    print()

    start_time = time.time()
    results: Dict[str, dict] = {}
    all_metrics: Dict[str, List[dict]] = {}
    raw_outputs: Dict[str, List[str]] = {}
    summaries: List[dict] = []

    # 逐一运行（串行，每个都能独占 CPU 准确测量）
    for mod, binary in binaries.items():
        print(f"[{mod}] 启动...", end=" ", flush=True)
        result = run_test(binary, args.duration)
        results[mod] = result

        metrics = parse_metrics(result["metrics_lines"])
        all_metrics[mod] = metrics
        raw_outputs[mod] = result["stdout_lines"]

        summary = compute_summary(mod, metrics, result["wall_duration_s"])
        summary["exit_code"] = result["exit_code"]
        summaries.append(summary)

        status = "✅" if result["exit_code"] in (0, -15, None) else f"❌ rc={result['exit_code']}"
        ops = summary.get("ops_per_sec", 0)
        print(f"{status}  {result['wall_duration_s']:.0f}s  {ops:,} ops/s  {len(metrics)} samples")

    total_s = time.time() - start_time

    # 保存报告
    report_dir = save_report(run_id, summaries, raw_outputs, all_metrics, args.duration)
    print(f"\n📊 报告已保存: {report_dir}")
    print(f"   summary.md  → {report_dir / 'summary.md'}")

    # 打印简明摘要
    print(f"\n{'='*60}")
    print(f"运行完成 — {run_id}")
    print(f"{'='*60}")
    print(f"{'模块':<20} {'ops/s':>12} {'lock/s':>10} {'延迟(us)':>10} {'状态':>6}")
    print(f"{'-'*20} {'-'*12} {'-'*10} {'-'*10} {'-'*6}")
    for s in summaries:
        status = "✅" if s.get("exit_code", 0) == 0 else "❌"
        print(f"{s['module']:<20} {s.get('ops_per_sec', 0):>12,} "
              f"{s.get('lock_ops_per_sec', 0):>10,} "
              f"{s.get('lock_avg_us', 0):>10.1f} "
              f"{status:>6}")
    print(f"{'='*60}")
    print(f"总耗时: {total_s:.0f}s ({total_s/60:.1f}min)")


if __name__ == "__main__":
    main()
