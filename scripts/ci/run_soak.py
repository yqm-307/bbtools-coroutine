#!/usr/bin/env python3
"""Jenkins 低资源持续疲劳测试（Soak）Harness。

单进程覆盖六个模块。脚本仅采集和判定，不修改主机资源限制。
正式 CLI 以 build 目录、测试时长、采样间隔和报告根目录为输入；
每段运行创建独立 UTC 子目录，绝不覆盖历史段。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import signal
import socket
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import TextIO

from ci_common import find_dump_files, utc_timestamp, write_json_atomic

REQUIRED_MODULES = ["comutex", "corwmutex", "cocond", "chan", "copool", "coroutine"]
REPORT_FILES = ["resource.jsonl", "metrics.jsonl", "summary.json", "summary.md", "raw.log"]
_METRIC_PREFIX = "FATIGUE_METRIC:"
_ANSI_RE = re.compile(r"\x1B(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\)|[()][A-Z0-9])")
_ERROR_PATTERNS = [
    ("assert", re.compile(r"Assert(?:ion)?", re.IGNORECASE)),
    ("segfault", re.compile(r"Segmentation fault", re.IGNORECASE)),
    ("asan", re.compile(r"AddressSanitizer", re.IGNORECASE)),
    ("error", re.compile(r"\bERROR\b", re.IGNORECASE)),
]
_OOM_PATTERNS = [
    re.compile(r"out of memory", re.IGNORECASE),
    re.compile(r"oom[- ]kill", re.IGNORECASE),
    re.compile(r"killed process", re.IGNORECASE),
]
_CLK_TCK = os.sysconf("SC_CLK_TCK")
_PAGE_SIZE = os.sysconf("SC_PAGE_SIZE")


@dataclass(frozen=True)
class Evaluation:
    status: str
    issues: list[str] = field(default_factory=list)


def parse_metric_line(line: str) -> dict | None:
    """解析 FATIGUE_METRIC JSON；非指标、坏 JSON、非对象返回 None。"""
    clean = _ANSI_RE.sub("", line.strip())
    if not clean.startswith(_METRIC_PREFIX):
        return None
    try:
        value = json.loads(clean[len(_METRIC_PREFIX):])
    except json.JSONDecodeError:
        return None
    return value if isinstance(value, dict) else None


def has_stall(samples: list[int | float], max_unchanged_intervals: int = 2) -> bool:
    """最后 N 个区间均无进展时返回 True；N 个区间需要 N+1 个样本。"""
    need = max_unchanged_intervals + 1
    return len(samples) >= need and len(set(samples[-need:])) == 1


def evaluate_function_metrics(metrics: dict, required_modules: list[str]) -> Evaluation:
    """判定模块完整性、errors 和进展；缺模块使用稳定状态 METRIC_MISSING。"""
    missing = [name for name in required_modules if name not in metrics]
    if missing:
        return Evaluation("METRIC_MISSING", [f"missing module(s): {', '.join(missing)}"])
    issues: list[str] = []
    for name in required_modules:
        value = metrics[name]
        if isinstance(value, list):
            history = value
            latest = {"ops_total": history[-1] if history else 0, "errors": 0}
        else:
            latest = value
            history = latest.get("ops_history", [latest.get("ops_total", 0)])
        errors = latest.get("errors", 0)
        if not isinstance(errors, (int, float)) or isinstance(errors, bool):
            issues.append(f"invalid errors: {name}")
        elif errors > 0:
            issues.append(f"metric errors: {name}={errors}")
        if has_stall(history):
            issues.append(f"stall: {name}")
    return Evaluation("FAIL", issues) if issues else Evaluation("OK")


def evaluate_resource(
    rss_mib: float,
    cpu_pct: float,
    memory_high: float | None,
    memory_max: float | None,
    cpu_high: float | None = None,
) -> Evaluation:
    """资源阈值判定：MemoryMax 为 FAIL，MemoryHigh/CPU high 为 WARN。"""
    if memory_max is not None and rss_mib > memory_max:
        return Evaluation("FAIL", [f"rss above max: {rss_mib} MiB > {memory_max} MiB"])
    issues: list[str] = []
    if memory_high is not None and rss_mib > memory_high:
        issues.append(f"rss above high: {rss_mib} MiB > {memory_high} MiB")
    if cpu_high is not None and cpu_pct > cpu_high:
        issues.append(f"cpu above high: {cpu_pct}% > {cpu_high}%")
    return Evaluation("WARN", issues) if issues else Evaluation("OK")


def _status_value(text: str, key: str) -> int | None:
    for line in text.splitlines():
        if line.startswith(key + ":"):
            try:
                return int(line.split(":", 1)[1].strip().split()[0])
            except (ValueError, IndexError):
                return None
    return None


def sample_proc(pid: int, prev: dict | None = None) -> dict | None:
    """读取 /proc 资源；PSS 从 smaps_rollup 获取，权限不足时为 None。"""
    proc = Path("/proc") / str(pid)
    try:
        stat = (proc / "stat").read_text(errors="replace")
        status = (proc / "status").read_text(errors="replace")
        fields = stat[stat.rindex(")") + 1:].split()
        utime, stime, rss_pages = int(fields[10]), int(fields[11]), int(fields[20])
    except (OSError, ValueError, IndexError):
        return None
    pss_kb = None
    try:
        pss_kb = _status_value((proc / "smaps_rollup").read_text(errors="replace"), "Pss")
    except OSError:
        pass
    now = time.monotonic()
    cpu_pct = 0.0
    if prev is not None:
        wall = now - prev["wall_ts"]
        if wall > 0:
            ticks = (utime + stime) - (prev["utime"] + prev["stime"])
            cpu_pct = max(0.0, ticks / _CLK_TCK / wall * 100.0)
    rss_kb = _status_value(status, "VmRSS")
    rss_mib = (rss_kb if rss_kb is not None else rss_pages * _PAGE_SIZE / 1024) / 1024
    return {
        "wall_ts": now,
        "utime": utime,
        "stime": stime,
        "rss_mib": round(rss_mib, 3),
        "pss_mib": round(pss_kb / 1024, 3) if pss_kb is not None else None,
        "threads": _status_value(status, "Threads"),
        "cpu_pct": round(cpu_pct, 3),
    }


def scan_error_hits(text: str) -> list[dict]:
    hits: list[dict] = []
    seen: set[tuple[str, str]] = set()
    for raw in text.splitlines():
        line = _ANSI_RE.sub("", raw).strip()
        for name, pattern in _ERROR_PATTERNS:
            if pattern.search(line) and (name, line) not in seen:
                seen.add((name, line))
                hits.append({"pattern": name, "line": line})
    return hits


def _compact_utc() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def _inside(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def create_run_dir(report_root: Path, repository_root: Path) -> Path:
    """创建受控唯一目录；任何 resolve 后越界路径都拒绝。"""
    allowed = (repository_root / "tests/reports/soak").resolve()
    requested = report_root.resolve()
    if not _inside(requested, allowed):
        raise ValueError(f"--report-dir 必须位于 {allowed} 内")
    requested.mkdir(parents=True, exist_ok=True)
    run_id = _compact_utc()
    for suffix in [""] + [f"-{index:02d}" for index in range(1, 100)]:
        candidate = requested / f"{run_id}{suffix}"
        try:
            candidate.mkdir()
            return candidate
        except FileExistsError:
            continue
    raise FileExistsError("同一秒的 Soak 报告目录已耗尽，拒绝覆盖")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="单进程六模块低资源 Soak Harness")
    parser.add_argument("--build-dir", type=Path, default=Path("build-soak"))
    parser.add_argument("--duration-seconds", "--duration", dest="duration_seconds", type=float, default=21600.0)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--resource-interval-seconds", "--sample-interval", dest="resource_interval_seconds", type=float, default=10.0)
    parser.add_argument("--metric-interval-seconds", "--metric-interval", dest="metric_interval_seconds", type=float, default=60.0)
    parser.add_argument("--report-dir", "--output-dir", dest="report_dir", type=Path, default=Path("tests/reports/soak"))
    parser.add_argument("--memory-high-mib", type=float)
    parser.add_argument("--memory-max-mib", type=float)
    parser.add_argument("--cpu-high-pct", type=float)
    parser.add_argument("--sigterm-grace-seconds", "--sigterm-grace", dest="sigterm_grace_seconds", type=float, default=10.0)
    return parser.parse_args(argv)


def validate_args(args: argparse.Namespace, repository_root: Path) -> Path:
    positive = {
        "--duration-seconds": args.duration_seconds,
        "--threads": args.threads,
        "--resource-interval-seconds": args.resource_interval_seconds,
        "--metric-interval-seconds": args.metric_interval_seconds,
        "--sigterm-grace-seconds": args.sigterm_grace_seconds,
    }
    for name, value in positive.items():
        if value <= 0:
            raise ValueError(f"{name} 必须大于 0")
    if args.memory_high_mib is not None and args.memory_max_mib is not None:
        if args.memory_high_mib > args.memory_max_mib:
            raise ValueError("MemoryHigh 不得大于 MemoryMax")
    build_dir = args.build_dir.resolve()
    binary = build_dir / "bin/benchmark_test/unified_stress"
    if not build_dir.is_dir() or not binary.is_file():
        raise ValueError(f"Soak 二进制不存在: {binary}")
    allowed = (repository_root / "tests/reports/soak").resolve()
    if not _inside(args.report_dir.resolve(), allowed):
        raise ValueError(f"--report-dir 必须位于 {allowed} 内")
    return binary


def _reader(stream: TextIO, stream_name: str, lines: list[str], metrics: list[dict], lock: threading.Lock) -> None:
    for line in stream:
        with lock:
            lines.append(f"[{stream_name}] {line}")
            metric = parse_metric_line(line)
            if metric is not None:
                metrics.append(metric)


def _histories(rows: list[dict]) -> tuple[dict[str, list[int | float]], dict[str, dict]]:
    histories: dict[str, list[int | float]] = {}
    latest: dict[str, dict] = {}
    for row in rows:
        name, ops = row.get("name"), row.get("ops_total")
        if name not in REQUIRED_MODULES or not isinstance(ops, (int, float)) or isinstance(ops, bool):
            continue
        histories.setdefault(name, []).append(ops)
        latest[name] = dict(row)
    for name, row in latest.items():
        row["ops_history"] = histories[name]
    return histories, latest


def _resource_summary(samples: list[dict]) -> dict:
    rss = [float(item["rss_mib"]) for item in samples]
    cpu = [float(item["cpu_pct"]) for item in samples]
    threads = [int(item["threads"]) for item in samples if item.get("threads") is not None]
    delta = rss[-1] - rss[0] if len(rss) >= 2 else 0.0
    monotonic = len(rss) >= 3 and all(right >= left for left, right in zip(rss, rss[1:])) and delta > 0
    return {
        "samples": len(samples),
        "max_rss_mib": max(rss) if rss else None,
        "max_cpu_pct": max(cpu) if cpu else None,
        "max_threads": max(threads) if threads else None,
        "rss_start_mib": rss[0] if rss else None,
        "rss_end_mib": rss[-1] if rss else None,
        "rss_delta_mib": round(delta, 3),
        "rss_monotonic_growth_observed": monotonic,
        "memory_note": "单段趋势仅为观测信号，不能单独证明内存泄漏",
    }


def run_soak(args: argparse.Namespace, binary: Path, run_dir: Path) -> tuple[dict, list[dict], list[dict], list[str]]:
    cmd = [str(binary), f"--threads={args.threads}", str(int(args.duration_seconds)), "0", "0"]
    env = dict(os.environ)
    env["INTERVAL"] = str(max(1, int(args.metric_interval_seconds)))
    proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
        errors="replace", start_new_session=True, env=env,
    )
    assert proc.stdout is not None and proc.stderr is not None
    raw_lines: list[str] = []
    metrics: list[dict] = []
    lock = threading.Lock()
    readers = [
        threading.Thread(target=_reader, args=(proc.stdout, "stdout", raw_lines, metrics, lock)),
        threading.Thread(target=_reader, args=(proc.stderr, "stderr", raw_lines, metrics, lock)),
    ]
    for thread in readers:
        thread.start()

    samples: list[dict] = []
    start = time.monotonic()
    last_sample = 0.0
    prev = None
    reached_target = False
    term_sent = False
    forced_kill = False
    while proc.poll() is None:
        now = time.monotonic()
        if prev is None or now - last_sample >= args.resource_interval_seconds:
            sample = sample_proc(proc.pid, prev)
            if sample is not None:
                sample["timestamp"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
                samples.append(sample)
                prev = sample
            last_sample = now
        if now - start >= args.duration_seconds:
            reached_target = True
            term_sent = True
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                proc.wait(timeout=args.sigterm_grace_seconds)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.wait()
                forced_kill = True
            break
        time.sleep(min(0.25, max(0.02, args.resource_interval_seconds / 4)))

    elapsed = time.monotonic() - start
    if not reached_target and elapsed >= args.duration_seconds - 0.1:
        reached_target = True
    for thread in readers:
        thread.join()
    proc.stdout.close()
    proc.stderr.close()

    histories, latest = _histories(metrics)
    function_eval = evaluate_function_metrics(latest, REQUIRED_MODULES)
    resources = _resource_summary(samples)
    resource_eval = evaluate_resource(
        resources["max_rss_mib"] or 0.0,
        resources["max_cpu_pct"] or 0.0,
        args.memory_high_mib,
        args.memory_max_mib,
        args.cpu_high_pct,
    )
    combined = "".join(raw_lines)
    oom_text = [pattern.pattern for pattern in _OOM_PATTERNS if pattern.search(combined)]
    if forced_kill:
        category = "forced_kill"
    elif not reached_target and proc.returncode == -signal.SIGKILL:
        category = "oom_suspected"
    elif not reached_target:
        category = "early_exit"
    elif term_sent and proc.returncode in (0, -signal.SIGTERM):
        category = "term_ok"
    elif proc.returncode is not None and proc.returncode < 0:
        category = "crash"
    elif proc.returncode not in (0, None):
        category = "failure"
    else:
        category = "pass"

    error_hits = scan_error_hits(combined)
    dumps = [str(path) for path in find_dump_files([Path.cwd(), binary.parent, run_dir])]
    issues = list(function_eval.issues) + list(resource_eval.issues)
    if category not in ("pass", "term_ok"):
        issues.append(f"process category: {category}")
    if error_hits:
        issues.append(f"error log hits: {len(error_hits)}")
    if dumps:
        issues.append(f"dump files: {len(dumps)}")
    if resources["rss_monotonic_growth_observed"]:
        issues.append("rss monotonic growth observed; needs multi-segment review")
    fail = function_eval.status in ("FAIL", "METRIC_MISSING") or resource_eval.status == "FAIL"
    fail = fail or category not in ("pass", "term_ok") or bool(error_hits) or bool(dumps)
    status = "FAIL" if fail else ("WARN" if resource_eval.status == "WARN" or resources["rss_monotonic_growth_observed"] else "PASS")
    report = {
        "run_id": run_dir.name,
        "timestamp": utc_timestamp(),
        "node": socket.gethostname(),
        "commit": _git_commit(),
        "pid": proc.pid,
        "command": cmd,
        "process": {
            "returncode": proc.returncode,
            "category": category,
            "target_duration_seconds": args.duration_seconds,
            "elapsed_seconds": round(elapsed, 3),
            "completed_target_duration": reached_target,
            "term_sent": term_sent,
            "forced_kill": forced_kill,
            "oom_evidence": oom_text,
            "oom_uncertainty": "SIGKILL 非 Harness 发出时仅标记 suspected；需结合内核日志确认" if category == "oom_suspected" else None,
        },
        "metrics": {
            "rows": len(metrics),
            "modules_seen": sorted(latest),
            "missing_modules": [name for name in REQUIRED_MODULES if name not in latest],
            "errors": sum(row.get("errors", 0) for row in latest.values() if isinstance(row.get("errors", 0), (int, float))),
        },
        "resources": resources,
        "function_evaluation": {"status": function_eval.status, "issues": function_eval.issues},
        "resource_evaluation": {"status": resource_eval.status, "issues": resource_eval.issues},
        "status": status,
        "exit_code": 1 if fail else 0,
        "issues": issues,
        "error_hits": error_hits,
        "dumps": dumps,
    }
    return report, samples, metrics, raw_lines


def _git_commit() -> str | None:
    try:
        result = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True, timeout=5)
        return result.stdout.strip() if result.returncode == 0 else None
    except (OSError, subprocess.TimeoutExpired):
        return None


def write_reports(report: dict, samples: list[dict], metrics: list[dict], raw_lines: list[str], run_dir: Path) -> None:
    (run_dir / "resource.jsonl").write_text("".join(json.dumps(row, ensure_ascii=False) + "\n" for row in samples), encoding="utf-8")
    (run_dir / "metrics.jsonl").write_text("".join(json.dumps(row, ensure_ascii=False) + "\n" for row in metrics), encoding="utf-8")
    (run_dir / "raw.log").write_text("".join(raw_lines), encoding="utf-8")
    (run_dir / "summary.md").write_text(render_markdown(report), encoding="utf-8")
    write_json_atomic(run_dir / "summary.json", report)


def render_markdown(report: dict) -> str:
    lines = [
        "# Soak Report", "", f"- status: {report['status']}", f"- run_id: {report['run_id']}",
        f"- process: {report['process']['category']}", f"- elapsed: {report['process']['elapsed_seconds']}s",
        f"- modules: {', '.join(report['metrics']['modules_seen']) or 'none'}",
        f"- metric errors: {report['metrics']['errors']}", f"- max RSS: {report['resources']['max_rss_mib']} MiB",
        f"- max CPU: {report['resources']['max_cpu_pct']}%", f"- max threads: {report['resources']['max_threads']}",
        f"- RSS delta: {report['resources']['rss_delta_mib']} MiB", f"- memory note: {report['resources']['memory_note']}",
        "", "## Issues", "",
    ]
    lines.extend(f"- {issue}" for issue in report["issues"])
    if not report["issues"]:
        lines.append("- none")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    repository_root = Path.cwd().resolve()
    try:
        binary = validate_args(args, repository_root)
        run_dir = create_run_dir(args.report_dir, repository_root)
        report, samples, metrics, raw_lines = run_soak(args, binary, run_dir)
        write_reports(report, samples, metrics, raw_lines, run_dir)
    except (ValueError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    print(f"soak status: {report['status']} (exit={report['exit_code']}), report: {run_dir}")
    return report["exit_code"]


if __name__ == "__main__":
    raise SystemExit(main())
