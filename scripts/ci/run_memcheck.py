#!/usr/bin/env python3
"""内存检测（Valgrind memcheck）值守入口 —— 失败必闭合（fail-closed）。

用法：
    python3 scripts/ci/run_memcheck.py --check-env
    python3 scripts/ci/run_memcheck.py \\
        --binary build-memcheck/bin/benchmark_test/mem_check_test \\
        --report-dir tests/ci-reports/memcheck --timeout 1800

判定口径（任一条不满足即 FAIL，绝不因“看起来像误报”放行）：
- 环境：valgrind 存在；--version 返回 0；能用真实 guest（/bin/true）探活通过
  （覆盖 ld.so 调试符号缺失导致的启动期 Fatal）；Boost 满足 1.90；目标存在且可执行。
- 进程：在 --timeout 内退出；退出码为 0（valgrind --error-exitcode=42 会把
  definite/indirect 泄漏计入退出码）；崩溃（信号）、超时、缺日志、空日志一律 FAIL。
- Valgrind 日志：含 ERROR SUMMARY 且 errors == 0；definitely lost == 0；
  indirectly lost == 0；出现 "Fatal error" / "Cannot continue" 一律 FAIL。
- 工作负载：stdout 必须出现**唯一一条** BBT_MEMCHECK_DONE，且七个必需计数键齐全、取值与
  EXPECTED_COUNTERS 固定期望逐项相等（缺键、重复键、多条互相冲突标记、键自洽但工作量缩水
  一律 FAIL）；出现 BBT_MEMCHECK_FAILED 一律 FAIL。

不使用抑制文件：definitely/indirectly lost 必须真实为 0，不允许用 suppression 掩盖。
possibly lost / still reachable 原样记录但不作门禁（分别来自存活 worker TLS 与
process-lifetime 单例），这不是抑制，是明确的判定口径。

报告写入 <report-dir>/：memcheck.log（Valgrind 原生日志）、memcheck.stdout.log、
memcheck.stderr.log、summary.json、summary.md。

进入判定路径即先清理该目录本轮的固定产物（<log-name>.log/.stdout.log/.stderr.log、
summary.json/md），并保证**必写** summary：前置校验失败（含 valgrind 缺失/loader 符号
不可用/Boost 不匹配/目标缺失）时不启动 workload，但仍落一份 FAIL summary —— 上一轮的
PASS 报告绝不能残留下来随 artifact 归档。`--check-env` 不写报告。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DONE_MARKER = "BBT_MEMCHECK_DONE"
FAILED_MARKER = "BBT_MEMCHECK_FAILED"
BOOST_REQUIRED = "1_90"

# Boost 头候选根目录：优先环境变量，其次常见安装位置（含 self-hosted runner 的 /opt/boost）
BOOST_ROOT_CANDIDATES = (
    "/opt/boost/include",
    "/usr/local/include",
    "/usr/include",
)

ERROR_SUMMARY_RE = re.compile(r"ERROR SUMMARY:\s*(\d+)\s+errors? from\s+(\d+)\s+contexts")
DEFINITELY_LOST_RE = re.compile(r"definitely lost:\s*([\d,]+)\s*bytes")
INDIRECTLY_LOST_RE = re.compile(r"indirectly lost:\s*([\d,]+)\s*bytes")
POSSIBLY_LOST_RE = re.compile(r"possibly lost:\s*([\d,]+)\s*bytes")
STILL_REACHABLE_RE = re.compile(r"still reachable:\s*([\d,]+)\s*bytes")
FATAL_MARKERS = ("valgrind:  Fatal error", "valgrind: Fatal error", "Cannot continue")
COUNTER_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=(\d+)/(\d+)")

# 完成协议的必需计数键与固定期望值：必须与 benchmark_test/mem_check_test.cc 的
# BBT_MEMCHECK_DONE 打印一致。只要求“每个键 num == total”不够——键齐全但整体缩水
# （如 inner=1/1）同样会被判 PASS，故这里固定期望值。
# runtime_drained=1 的语义边界：代表“所有借出的栈已归还（~Context 已进入 FINAL 释放
# 路径）”，且按 #379 采用的 A —— ~Context 函数体先清空 m_onyield_callback / m_user_main
# 再 Release(m_stack) —— 蕴含“这两个 callable 及其最后持有的捕获对象已同步析构”；但不
# 证明外部其它 owner 的资源、worker 侧异步物理清理与 delete 已返回（见 docs/ci-guide.md
# §4），此处只做如实记录，不据此宣称“完整释放”。
EXPECTED_COUNTERS = {
    "inner": (10000, 10000),
    "lock": (4000, 4000),
    "chan_read": (500000, 500000),
    "writer_done": (50, 50),
    "failures": (0, 0),
    "runtime_drained": (1, 1),
    "stack_pool_drained": (1, 1),
}


class Verdict:
    def __init__(self) -> None:
        self.checks: list[dict] = []
        self.metrics: dict = {}

    def add(self, name: str, ok: bool, detail: str) -> None:
        self.checks.append({"name": name, "ok": bool(ok), "detail": detail})

    @property
    def ok(self) -> bool:
        return all(c["ok"] for c in self.checks)

    def failed(self) -> list[dict]:
        return [c for c in self.checks if not c["ok"]]


def _parse_bytes(value: str) -> int:
    return int(value.replace(",", ""))


def find_boost_root() -> tuple[str | None, str]:
    """返回 (匹配 1.90 的 include 根, 说明)；找不到返回 (None, 原因)。"""
    env_root = os.environ.get("BBT_BOOST_INCLUDE_ROOT", "")
    candidates = ([env_root] if env_root else []) + list(BOOST_ROOT_CANDIDATES)
    seen: list[str] = []
    for root in candidates:
        header = Path(root) / "boost" / "version.hpp"
        if not header.is_file():
            seen.append(f"{root}(缺 version.hpp)")
            continue
        m = re.search(r'#define\s+BOOST_LIB_VERSION\s+"([^"]+)"',
                      header.read_text(errors="replace"))
        if not m:
            seen.append(f"{root}(无法解析)")
            continue
        if m.group(1) == BOOST_REQUIRED:
            return root, f"BOOST_LIB_VERSION={m.group(1)} @ {root}"
        seen.append(f"{root}={m.group(1)}")
    return None, f"未找到 Boost {BOOST_REQUIRED}；检查过：{', '.join(seen)}"


def check_valgrind_loadable(valgrind: str) -> tuple[bool, str]:
    """用真实 guest（/bin/true）探活，覆盖 ld.so 符号缺失导致的启动期 Fatal。"""
    guest = "/bin/true"
    if not Path(guest).exists():
        return False, f"探活 guest 不存在：{guest}"
    with tempfile.TemporaryDirectory() as tmp:
        log = Path(tmp) / "probe.log"
        try:
            proc = subprocess.run(
                [valgrind, "--tool=memcheck", f"--log-file={log}", guest],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            return False, f"valgrind 探活失败：{exc}"
        text = log.read_text(errors="replace") if log.exists() else ""
        if any(marker in text for marker in FATAL_MARKERS):
            return False, "valgrind 启动即 Fatal（通常缺少 libc6-dbg / loader 调试符号）"
        if proc.returncode != 0:
            return False, f"valgrind 探活返回码 {proc.returncode}：{text.strip()[:400]}"
        return True, "valgrind 可正常启动并运行 guest"


def preflight(args: argparse.Namespace, verdict: Verdict) -> str | None:
    """返回 valgrind 可执行路径；失败返回 None（已写入 verdict）。"""
    valgrind = args.valgrind or shutil.which("valgrind") or ""
    if not valgrind or not Path(valgrind).exists():
        verdict.add(
            "valgrind-present", False,
            "未找到 valgrind 可执行文件；内存检测 runner 必须预装 valgrind"
            "（含 libc6-dbg，见 docs/ci-guide.md §4 跨仓镜像依赖）",
        )
        return None
    verdict.add("valgrind-present", True, valgrind)

    try:
        proc = subprocess.run([valgrind, "--version"], stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=60)
        out = proc.stdout.decode(errors="replace").strip()
        verdict.add("valgrind-version", proc.returncode == 0 and bool(out),
                    out or f"<空输出 rc={proc.returncode}>")
    except (OSError, subprocess.TimeoutExpired) as exc:
        verdict.add("valgrind-version", False, f"执行 --version 失败：{exc}")

    ok, detail = check_valgrind_loadable(valgrind)
    verdict.add("valgrind-loader-symbols", ok, detail)

    root, detail = find_boost_root()
    verdict.add("boost-1.90", root is not None, detail)

    if args.check_env:
        return valgrind

    binary = Path(args.binary)
    if not binary.is_file():
        verdict.add("binary-present", False, f"目标不存在：{binary}")
    elif not os.access(binary, os.X_OK):
        verdict.add("binary-present", False, f"目标不可执行：{binary}")
    else:
        verdict.add("binary-present", True, str(binary))

    return valgrind


def build_valgrind_command(args: argparse.Namespace, valgrind: str, log: Path) -> list[str]:
    """构造 memcheck 命令行。不使用任何抑制文件。"""
    return [
        valgrind,
        "--tool=memcheck",
        "--leak-check=full",
        "--show-leak-kinds=all",
        # 只让 definite/indirect 计入错误/退出码与 ERROR SUMMARY；possibly/still reachable
        # 原样记录但不作门禁（worker TLS / process-lifetime 单例的已知来源）。
        "--errors-for-leak-kinds=definite,indirect",
        f"--error-exitcode={args.error_exitcode}",
        f"--log-file={log}",
        str(args.binary),
    ]


def parse_valgrind(log_text: str) -> dict:
    metrics: dict = {}
    for key, regex in (
        ("definitely_lost", DEFINITELY_LOST_RE),
        ("indirectly_lost", INDIRECTLY_LOST_RE),
        ("possibly_lost", POSSIBLY_LOST_RE),
        ("still_reachable", STILL_REACHABLE_RE),
    ):
        m = regex.search(log_text)
        if m:
            metrics[key] = _parse_bytes(m.group(1))
    m = ERROR_SUMMARY_RE.search(log_text)
    if m:
        metrics["errors"] = int(m.group(1))
    return metrics


def check_workload_marker(verdict: Verdict, stdout_text: str, stderr_text: str = "") -> None:
    """完成标记必须唯一出现，且七个必需计数键与 EXPECTED_COUNTERS 固定期望完全一致。"""
    if FAILED_MARKER in stdout_text or FAILED_MARKER in stderr_text:
        verdict.add("workload-completed", False, f"程序自报未完成：{FAILED_MARKER}")
        return
    marker_lines = [ln.strip() for ln in stdout_text.splitlines() if DONE_MARKER in ln]
    if not marker_lines:
        verdict.add("workload-completed", False, f"stdout 缺少完成标记 {DONE_MARKER}")
        return
    if len(marker_lines) > 1:
        verdict.add("workload-completed", False,
                    f"stdout 出现 {len(marker_lines)} 条 {DONE_MARKER}，互相冲突无法判定："
                    f"{marker_lines}")
        return
    marker_line = marker_lines[0]
    pairs = [(n, int(a), int(b)) for n, a, b in COUNTER_RE.findall(marker_line)]
    names = [n for n, _, _ in pairs]
    dup = sorted({n for n in names if names.count(n) > 1})
    if dup:
        verdict.add("workload-completed", False,
                    f"完成标记计数键重复 {dup}（取值互相冲突）：{marker_line}")
        return
    got = {n: (a, b) for n, a, b in pairs}
    missing = [k for k in EXPECTED_COUNTERS if k not in got]
    if missing:
        verdict.add("workload-completed", False,
                    f"完成标记缺少必需计数 {missing}：{marker_line or '<空行>'}")
        return
    bad = [f"{n}={got[n][0]}/{got[n][1]}(期望 {e}/{t})"
           for n, (e, t) in EXPECTED_COUNTERS.items() if got[n] != (e, t)]
    verdict.add("workload-completed", not bad,
                ", ".join(f"{n}={a}/{b}" for n, a, b in pairs)
                + (f"；未达标 {bad}" if bad else ""))


def run_valgrind(args: argparse.Namespace, verdict: Verdict, valgrind: str,
                 report_dir: Path) -> dict:
    log = report_dir / f"{args.log_name}.log"
    stdout_log = report_dir / f"{args.log_name}.stdout.log"
    stderr_log = report_dir / f"{args.log_name}.stderr.log"

    cmd = build_valgrind_command(args, valgrind, log)
    timed_out = False
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=args.timeout)
        rc, out, err = proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        rc, out, err = None, exc.stdout or b"", exc.stderr or b""
    except OSError as exc:
        verdict.add("run", False, f"执行 valgrind 失败：{exc}")
        return {}

    stdout_log.write_bytes(out)
    stderr_log.write_bytes(err)
    log_text = log.read_text(errors="replace") if log.is_file() else ""

    if timed_out:
        verdict.add("run", False, f"超过 {args.timeout}s 未退出（超时）")
        return {"timed_out": True, "log_text": log_text, "stdout": out.decode(errors="replace")}

    if rc is not None and rc < 0:
        verdict.add("run", False, f"进程被信号终止（rc={rc}，疑似崩溃）")
    else:
        verdict.add("run", rc == 0, f"valgrind 退出码 {rc}（0 才通过）")

    if not log_text.strip():
        verdict.add("valgrind-log", False, "Valgrind 日志缺失或为空")
        return {"log_text": log_text, "stdout": out.decode(errors="replace")}

    fatal = [m for m in FATAL_MARKERS if m in log_text]
    verdict.add("valgrind-log", not fatal, "无 Fatal 标记" if not fatal else f"出现 {fatal}")

    metrics = parse_valgrind(log_text)
    verdict.metrics.update(metrics)

    m = ERROR_SUMMARY_RE.search(log_text)
    if not m:
        verdict.add("error-summary", False, "日志缺少 ERROR SUMMARY，无法判定")
    else:
        verdict.add("error-summary", int(m.group(1)) == 0,
                    f"errors={m.group(1)}, contexts={m.group(2)}")

    for key, label in (("definitely_lost", "definitely lost"),
                       ("indirectly_lost", "indirectly lost")):
        if key not in metrics:
            verdict.add(key, False, f"日志缺少 {label} 统计")
        else:
            verdict.add(key, metrics[key] == 0, f"{label}={metrics[key]} bytes")

    stdout_text = out.decode(errors="replace")
    stderr_text = err.decode(errors="replace")
    check_workload_marker(verdict, stdout_text, stderr_text)
    return {"log_text": log_text, "stdout": stdout_text, "stderr": stderr_text}


def write_reports(report_dir: Path, verdict: Verdict, args: argparse.Namespace) -> None:
    summary = {
        "verdict": "PASS" if verdict.ok else "FAIL",
        "binary": str(args.binary),
        "timeout_s": args.timeout,
        "suppressions": None,
        "metrics": verdict.metrics,
        "checks": verdict.checks,
    }
    (report_dir / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2))

    lines = [
        "# 内存检测 summary",
        "",
        f"- 结论：**{summary['verdict']}**",
        f"- 目标：`{summary['binary']}`",
        "- 抑制文件：无（definitely/indirectly lost 必须真实为 0）",
        "",
        "## 指标",
        "",
    ]
    for key in ("errors", "definitely_lost", "indirectly_lost", "possibly_lost",
                "still_reachable"):
        if key in verdict.metrics:
            lines.append(f"- {key}: {verdict.metrics[key]}")
    lines += ["", "## 检查项", ""]
    for c in verdict.checks:
        lines.append(f"- [{'x' if c['ok'] else ' '}] {c['name']}：{c['detail']}")
    (report_dir / "summary.md").write_text("\n".join(lines) + "\n")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Valgrind memcheck 值守入口（fail-closed）")
    parser.add_argument("--binary", default="", help="被测可执行文件")
    parser.add_argument("--report-dir", default="tests/ci-reports/memcheck")
    parser.add_argument("--timeout", type=int, default=1800, help="整体超时（秒）")
    parser.add_argument("--valgrind", default="", help="valgrind 可执行文件路径")
    parser.add_argument("--error-exitcode", type=int, default=42)
    parser.add_argument("--log-name", default="memcheck")
    parser.add_argument("--check-env", action="store_true", help="只做环境前置校验，不运行目标")
    args = parser.parse_args(argv)

    verdict = Verdict()
    report_dir = Path(args.report_dir)
    if not args.check_env:
        # 进入判定路径先清掉本目录上一轮的固定产物：旧日志/旧 PASS summary 绝不能
        # 充当本轮证据（只删本轮已知文件名，不做 rm -rf）。
        report_dir.mkdir(parents=True, exist_ok=True)
        for name in (f"{args.log_name}.log", f"{args.log_name}.stdout.log",
                     f"{args.log_name}.stderr.log", "summary.json", "summary.md"):
            (report_dir / name).unlink(missing_ok=True)

    valgrind = preflight(args, verdict)

    if not args.check_env:
        # 前置校验未全绿时不启动 workload（否则只是一次注定无意义的运行），但仍写 FAIL summary
        if verdict.ok and valgrind:
            run_valgrind(args, verdict, valgrind, report_dir)
        write_reports(report_dir, verdict, args)

    for check in verdict.checks:
        print(f"[{'OK' if check['ok'] else 'FAIL'}] {check['name']}: {check['detail']}")
    print(f"MEMCHECK VERDICT: {'PASS' if verdict.ok else 'FAIL'}")
    if not verdict.ok:
        for check in verdict.failed():
            print(f"::error::内存检测失败 —— {check['name']}: {check['detail']}")
    return 0 if verdict.ok else 1


if __name__ == "__main__":
    sys.exit(main())
