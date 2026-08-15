#!/usr/bin/env python3
"""Jenkins 定时 Smoke Harness：干净构建 + CTest smoke 测试 + 报告生成。

依次执行：清空构建目录 -> CMake 配置（Ninja）-> 并行构建 ->
ctest -L <label>（默认 smoke，支持时附加 --output-junit）。全程记录每条命令的
退出码/分类/耗时，收集系统信息，扫描 core/dump 文件与 stdout/stderr
中的错误模式（Assert / Segmentation fault / AddressSanitizer / ERROR），
最后输出 summary.json / summary.md / stdout.log / stderr.log / commands.json。

任一命令超时、崩溃、失败，或发现 dump 文件、错误模式命中时以非 0 退出。
只依赖标准库与 ci_common，不写死任何开发机路径。

用法示例：
    python3 scripts/ci/run_smoke.py --build-dir build-ci-smoke --timeout-seconds 900
"""

import argparse
import re
import shlex
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path

from ci_common import (
    CommandResult,
    find_dump_files,
    run_command,
    utc_timestamp,
    write_json_atomic,
)

# 错误模式：命中任一即视为 smoke 失败。按简报要求的大小写形式匹配。
_ERROR_PATTERNS: list[tuple[str, re.Pattern[str]]] = [
    ("assert", re.compile(r"Assert")),
    ("segfault", re.compile(r"Segmentation fault")),
    ("asan", re.compile(r"AddressSanitizer")),
    ("error", re.compile(r"ERROR")),
]

_CACHE_SUMMARY_KEYS = (
    "CMAKE_BUILD_TYPE",
    "CMAKE_CXX_COMPILER",
    "CMAKE_GENERATOR",
    "CMAKE_CXX_FLAGS",
    "NEED_TEST",
    "NEED_BENCHMARK",
)

# CTest label 作为 argv 单项传递；仍限制 grammar，避免空表达式、控制字符和
# shell 元字符进入 Jenkins 日志/报告，也让可调参数保持单一 label 语义。
_TEST_LABEL_RE = re.compile(r"^[A-Za-z0-9_.-]+$")

# ANSI 转义序列：CSI（\x1b[31m）、OSC（\x1b]0;title\x07）、字符集选择
# （\x1b(B）。彩色日志若不先剥离，模式可能被转义序列截断导致漏报。
_ANSI_ESCAPE_RE = re.compile(
    r"\x1b\[[0-9;?]*[A-Za-z]"
    r"|\x1b\][^\x07]*(?:\x07|\x1b\\)"
    r"|\x1b[()][A-Za-z0-9]"
)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Jenkins 定时 Smoke Harness")
    parser.add_argument("--source-dir", type=Path, default=Path("."),
                        help="仓库根目录（默认当前目录）")
    parser.add_argument("--build-dir", type=Path, default=Path("build-ci-smoke"),
                        help="CMake 构建目录（每次运行前清空）")
    parser.add_argument("--build-type", default="Debug",
                        help="CMAKE_BUILD_TYPE（默认 Debug）")
    parser.add_argument("--timeout-seconds", type=float, default=900.0,
                        help="单条命令超时秒数，超时先 SIGTERM 再 SIGKILL")
    parser.add_argument("--test-label", default="smoke",
                        help="CTest label（默认 smoke，仅允许字母/数字/._-）")
    parser.add_argument("--report-dir", type=Path, default=None,
                        help="报告输出目录（默认 tests/reports/smoke/<UTC 时间戳>）")
    return parser.parse_args(argv)


def validate_args(args: argparse.Namespace) -> None:
    """校验路径安全：拒绝把构建目录指向仓库根、其祖先、文件系统根、
    用户主目录，或主目录下但不在仓库内的任何目录。

    build 目录会被整体清空重建：指向 source 本身/祖先/根会误删源码；
    指向 home 或 home 下但 source 外的目录会误删用户数据。仓库位于
    home 下是常态（如 ~/Code/...），因此 build 只要落在 source 内就放行，
    即使其绝对路径位于 home 下。
    """
    if not args.source_dir.is_dir():
        sys.exit(f"error: --source-dir 不是目录: {args.source_dir}")
    source = args.source_dir.resolve()
    build = args.build_dir.resolve()
    home = Path.home().resolve()
    if build == source:
        sys.exit(f"error: --build-dir 不能等于 --source-dir: {build}")
    if source.is_relative_to(build):
        sys.exit(f"error: --build-dir 不能是 --source-dir 的祖先目录: {build}")
    if build == Path(build.anchor):
        sys.exit(f"error: --build-dir 不能是文件系统根: {build}")
    if build == home or home.is_relative_to(build):
        sys.exit(f"error: --build-dir 不能指向用户主目录或其祖先: {build}")
    if build.is_relative_to(home) and not build.is_relative_to(source):
        # home 下但 source 外的目录会被 rmtree 整体删除，必须拒绝。
        sys.exit(f"error: --build-dir 位于用户主目录下但不在 --source-dir 内: {build}")
    if args.timeout_seconds <= 0 or args.timeout_seconds > 1200:
        sys.exit("error: --timeout-seconds 必须在 (0, 1200] 范围内")
    if not _TEST_LABEL_RE.fullmatch(args.test_label):
        sys.exit("error: --test-label 仅允许字母、数字、点、下划线和连字符")


def compact_utc_timestamp() -> str:
    """紧凑 UTC 时间戳（20260815T120000Z），用于默认报告目录名。"""
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def scan_error_hits(text: str) -> list[dict[str, str]]:
    """扫描文本中的错误模式，返回命中记录 [{pattern, line}]（去重、保序）。

    每行先剥离 ANSI 转义序列再匹配，避免彩色日志漏报；命中行保存剥离
    后的干净文本。
    """
    hits: list[dict[str, str]] = []
    seen: set[tuple[str, str]] = set()
    for line in text.splitlines():
        clean = _ANSI_ESCAPE_RE.sub("", line)
        for name, pattern in _ERROR_PATTERNS:
            if pattern.search(clean):
                key = (name, clean.strip())
                if key not in seen:
                    seen.add(key)
                    hits.append({"pattern": name, "line": clean.strip()})
    return hits


def collect_system_info(source_dir: Path, build_dir: Path, timeout: float) -> dict:
    """收集内核、CMake/编译器版本与 CMake cache 摘要；单项失败不致命。"""
    info: dict[str, object] = {}
    kernel = run_command(["uname", "-a"], timeout=timeout)
    info["kernel"] = kernel.stdout.strip() or kernel.stderr.strip() or "unavailable"

    cmake_version = run_command(["cmake", "--version"], timeout=timeout)
    info["cmake_version"] = (
        cmake_version.stdout.splitlines()[0] if cmake_version.stdout else "unavailable"
    )

    cache_path = build_dir / "CMakeCache.txt"
    cache: dict[str, str] = {}
    if cache_path.is_file():
        for raw in cache_path.read_text(errors="replace").splitlines():
            if "=" not in raw:
                continue
            # cache 行形如 "CMAKE_BUILD_TYPE:STRING=Debug"，键在 ":" 之前。
            key = raw.split("=", 1)[0].split(":", 1)[0]
            value = raw.split("=", 1)[1]
            if key in _CACHE_SUMMARY_KEYS:
                cache[key] = value
    info["cmake_cache"] = cache

    compiler = cache.get("CMAKE_CXX_COMPILER", "")
    if compiler:
        version = run_command([compiler, "--version"], timeout=timeout)
        info["compiler_version"] = (
            version.stdout.splitlines()[0] if version.stdout else "unavailable"
        )
    else:
        info["compiler_version"] = "unavailable"
    return info


def build_command_list(args: argparse.Namespace, junit_supported: bool) -> list[list[str]]:
    """按简报契约组装配置/构建/测试命令，返回 argv 数组（保持数组形态）。"""
    source = str(args.source_dir.resolve())
    build = str(args.build_dir.resolve())
    configure = [
        "cmake", "-S", source, "-B", build, "-G", "Ninja",
        "-DNEED_TEST=ON", "-DNEED_BENCHMARK=ON",
        f"-DCMAKE_BUILD_TYPE={args.build_type}",
    ]
    build_cmd = ["cmake", "--build", build, "--parallel"]
    ctest_cmd = [
        "ctest", "--test-dir", build, "--output-on-failure", "-L", args.test_label,
    ]
    if junit_supported:
        # CTest 把相对路径相对 --test-dir 解析，必须传绝对路径才能落到报告目录。
        ctest_cmd += ["--output-junit", str((args.report_dir / "ctest.xml").resolve())]
    return [configure, build_cmd, ctest_cmd]


def resolve_junit_status(supported: bool, report_dir: Path) -> dict:
    """汇总 JUnit 探测结果：不支持时 path 为 None，不假装生成 XML；
    支持时校验 ctest.xml 是否真实生成，供调用方决定是否判失败。"""
    if not supported:
        return {"supported": False, "path": None, "generated": False}
    path = report_dir / "ctest.xml"
    return {"supported": True, "path": str(path), "generated": path.is_file()}


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    validate_args(args)
    args.report_dir = args.report_dir or Path(
        f"tests/reports/smoke/{compact_utc_timestamp()}"
    )
    report_dir = args.report_dir
    report_dir.mkdir(parents=True, exist_ok=True)

    commands: list[dict] = []
    all_stdout: list[str] = []
    all_stderr: list[str] = []

    def execute(label: str, cmd: list[str]) -> CommandResult:
        """执行命令并记录结果与输出。command 用 shlex.join 做 shell-safe 展示，
        argv 保留实际参数数组。"""
        result = run_command(cmd, timeout=args.timeout_seconds)
        commands.append(
            {
                "label": label,
                "argv": cmd,
                "command": shlex.join(cmd),
                "exit_code": result.returncode,
                "category": result.category,
                "timed_out": result.timed_out,
                "duration_seconds": round(result.duration, 3),
            }
        )
        all_stdout.append(f"### {label}: {shlex.join(cmd)}\n{result.stdout}")
        all_stderr.append(f"### {label}: {shlex.join(cmd)}\n{result.stderr}")
        return result

    def record_skipped(label: str, cmd: list[str]) -> None:
        """前序阶段失败后短路：后续阶段不再执行，但在报告里留痕为 skipped。"""
        commands.append(
            {
                "label": label,
                "argv": cmd,
                "command": shlex.join(cmd),
                "exit_code": None,
                "category": "skipped",
                "timed_out": False,
                "duration_seconds": 0.0,
            }
        )
        all_stdout.append(f"### {label}: {shlex.join(cmd)}\n(skipped: 前序阶段失败)\n")
        all_stderr.append(f"### {label}: {shlex.join(cmd)}\n(skipped: 前序阶段失败)\n")

    # 探测 CTest 是否支持 --output-junit：支持写 ctest.xml，不支持保留
    # Testing/ 目录与文本结果，不假装生成 JUnit XML。
    help_result = run_command(["ctest", "--help"], timeout=args.timeout_seconds)
    junit_supported = "--output-junit" in (help_result.stdout + help_result.stderr)

    # 干净构建：清空构建目录，避免旧产物污染结果。
    build_dir = args.build_dir.resolve()
    if build_dir.exists():
        shutil.rmtree(build_dir)

    # 阶段短路：configure 失败后不再 build/ctest，build 失败后不再 ctest；
    # 未执行的阶段以 category=skipped 记录，避免失败时三倍超时空耗。
    executed: list[CommandResult] = []
    for label, cmd in zip(
        ("configure", "build", "ctest"), build_command_list(args, junit_supported)
    ):
        if executed and executed[-1].category != "pass":
            record_skipped(label, cmd)
            continue
        executed.append(execute(label, cmd))

    system = collect_system_info(args.source_dir, args.build_dir, args.timeout_seconds)

    dumps = [str(p) for p in find_dump_files([args.source_dir, build_dir])]

    scanned_text = "\n".join(r.stdout + r.stderr for r in executed)
    error_hits = scan_error_hits(scanned_text)

    junit = resolve_junit_status(junit_supported, report_dir)
    # ctest 真实执行且通过、但 ctest.xml 缺失：JUnit 契约被破坏，明确判失败。
    junit_missing = (
        junit_supported
        and len(executed) == 3
        and executed[-1].category == "pass"
        and not junit["generated"]
    )

    failed = any(r.category != "pass" for r in executed)
    exit_code = 1 if (failed or dumps or error_hits or junit_missing) else 0
    status = "pass" if exit_code == 0 else "fail"

    summary = {
        "status": status,
        "exit_code": exit_code,
        "timestamp": utc_timestamp(),
        "args": {
            "source_dir": str(args.source_dir),
            "build_dir": str(args.build_dir),
            "build_type": args.build_type,
            "timeout_seconds": args.timeout_seconds,
        },
        "junit": junit,
        "skipped_stages": [
            c["label"] for c in commands if c["category"] == "skipped"
        ],
        "system": system,
        "commands": commands,
        "dumps": dumps,
        "error_hits": error_hits,
    }
    write_json_atomic(report_dir / "summary.json", summary)
    write_json_atomic(report_dir / "commands.json", commands)
    (report_dir / "stdout.log").write_text("\n".join(all_stdout), encoding="utf-8")
    (report_dir / "stderr.log").write_text("\n".join(all_stderr), encoding="utf-8")
    (report_dir / "summary.md").write_text(
        render_markdown(summary), encoding="utf-8"
    )

    print(f"smoke status: {status} (exit={exit_code}), report: {report_dir}")
    return exit_code


def render_markdown(summary: dict) -> str:
    """把 summary dict 渲染成人类可读的 markdown 报告。"""
    lines = [
        "# Smoke Harness Report",
        "",
        f"- status: {summary['status']}",
        f"- timestamp: {summary['timestamp']}",
        f"- exit code: {summary['exit_code']}",
        "",
        "## Commands",
        "",
        "| label | exit_code | category | timed_out | duration(s) |",
        "|---|---|---|---|---|",
    ]
    for c in summary["commands"]:
        lines.append(
            f"| {c['label']} | {c['exit_code']} | {c['category']} | "
            f"{c['timed_out']} | {c['duration_seconds']} |"
        )
    lines += ["", "## System", ""]
    for key, value in summary["system"].items():
        lines.append(f"- {key}: {value}")
    lines += ["", "## Dumps", ""]
    lines += [f"- {p}" for p in summary["dumps"]] or ["- none"]
    lines += ["", "## Error hits", ""]
    lines += [f"- [{h['pattern']}] {h['line']}" for h in summary["error_hits"]] or ["- none"]
    lines += ["", "## JUnit", ""]
    junit = summary["junit"]
    lines.append(f"- supported: {junit['supported']}")
    if junit["supported"]:
        lines.append(f"- path: {junit['path']}")
        if junit["generated"]:
            lines.append("- generated: yes")
        else:
            lines.append("- generated: NO（ctest.xml 缺失，视为 smoke 失败）")
    else:
        lines.append("- CTest 不支持 --output-junit，保留 Testing/ 目录与文本结果")
    skipped = summary.get("skipped_stages")
    if skipped:
        lines += ["", "## Skipped stages", ""]
        lines += [f"- {label}" for label in skipped]
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    sys.exit(main())
