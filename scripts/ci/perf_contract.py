#!/usr/bin/env python3
"""统一性能指标 Schema 与基线比较契约（任务 4）。

职责：
- 固定统一报告/模块指标字段集与合法判定值；
- 环境指纹采集与比较（指纹不一致只允许 NO_COMPARABLE_BASELINE，
  绝不转成 PASS）；
- 模块吞吐退化判定（WARN/FAIL 阈值 + gate 语义）；
- 统一 JSON 报告构造/校验与 Markdown 输出。

只依赖标准库与同包 ci_common（run_command / CommandResult 契约）。
纯函数为主：比较与校验不触碰文件系统或外部进程；唯一副作用函数是
collect_environment_fingerprint（读 /proc 与探测编译器版本）与
write_markdown_report（写文件），供脚本层在边界调用。
"""

import json
import os
import platform
from dataclasses import dataclass, field
from pathlib import Path

from ci_common import run_command

SCHEMA_VERSION = 1

REQUIRED_TOP_LEVEL = {
    "schema_version", "repository", "commit", "base_commit",
    "environment", "build", "parameters", "modules", "verdict",
}

REQUIRED_MODULE_FIELDS = {
    "ops_total", "ops_per_sec", "errors", "elapsed_s",
}

VALID_VERDICTS = {
    "PASS", "WARN", "FAIL", "UNSTABLE",
    "NO_COMPARABLE_BASELINE", "METRIC_INVALID",
}

# 吞吐退化阈值（ADR D6）：默认 10% WARN / 20% FAIL；CoCond 放宽到 30%/40%。
THRESHOLD_WARN = 10
THRESHOLD_FAIL = 20
THRESHOLD_COCOND = 30

# 环境指纹比较键：任一不一致 → NO_COMPARABLE_BASELINE。
# modules/durations 属于运行范围而非机器指纹，不参与比较（单模块 PR 检查
# 与全量基线记录的 modules 列表必然不同，比较它们会让 PR 永远无法对比）。
FINGERPRINT_KEYS = (
    "agent", "node", "cpu_model", "logical_cores", "mem_total_kb",
    "compiler", "cmake_version", "ninja_version", "build_type",
    "cmake_args", "threads",
)


@dataclass(frozen=True)
class CompareResult:
    """比较结果：status 为 VALID_VERDICTS 之一，detail 携带原因上下文。"""

    status: str
    module: str = ""
    old_ops: float = 0.0
    new_ops: float = 0.0
    delta_pct: float = 0.0
    detail: dict = field(default_factory=dict)


@dataclass(frozen=True)
class ValidationResult:
    """校验结果：status + 缺失字段列表。"""

    status: str
    missing: tuple = ()


def compare_environment(old_env, new_env) -> CompareResult:
    """比较两份环境指纹。

    任一 FINGERPRINT_KEYS 字段缺失或不一致 → NO_COMPARABLE_BASELINE。
    指纹比较失败只能产生 NO_COMPARABLE_BASELINE，不能转成 PASS。
    """
    if not old_env or not new_env:
        return CompareResult(
            status="NO_COMPARABLE_BASELINE",
            detail={"reason": "missing_environment"},
        )
    diffs = [
        key for key in FINGERPRINT_KEYS
        if old_env.get(key) != new_env.get(key)
    ]
    if diffs:
        return CompareResult(
            status="NO_COMPARABLE_BASELINE",
            detail={"reason": "environment_mismatch", "keys": diffs},
        )
    return CompareResult(status="PASS")


def validate_module_metrics(metrics) -> ValidationResult:
    """校验模块指标是否含全部 REQUIRED_MODULE_FIELDS。"""
    if not isinstance(metrics, dict):
        return ValidationResult(
            status="METRIC_INVALID", missing=tuple(sorted(REQUIRED_MODULE_FIELDS))
        )
    missing = tuple(sorted(REQUIRED_MODULE_FIELDS - set(metrics)))
    if missing:
        return ValidationResult(status="METRIC_INVALID", missing=missing)
    return ValidationResult(status="PASS")


def normalize_module_metrics(raw):
    """把 FATIGUE_METRIC 原始字段规整为统一模块指标。

    保留原始字段透传（lock_ops、chan_reads 等模块专属字段），剔除
    name/ts 这类采集元数据；ops_per_sec 由 ops_total / elapsed_s 计算。
    缺必需字段、数值类型非法（errors/延迟字段为字符串、bool 等）或
    elapsed_s <= 0 时返回 None，由调用方按 METRIC_INVALID 或 error 处理，
    避免下游（errors>0 比较、延迟除法）出现 TypeError。
    """
    if not isinstance(raw, dict):
        return None
    if not {"ops_total", "errors", "elapsed_s"} <= set(raw):
        return None
    # 必需数值字段：bool 是 int 子类，一并拒绝
    for key in ("ops_total", "errors", "elapsed_s"):
        value = raw[key]
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            return None
    elapsed = raw["elapsed_s"]
    if elapsed <= 0:
        return None
    # 延迟字段可选；若出现必须为数值（check_latency 会做除法）
    for key in ("lock_avg_us", "wlock_avg_us", "cond_avg_us"):
        value = raw.get(key)
        if value is not None and (
            isinstance(value, bool) or not isinstance(value, (int, float))
        ):
            return None

    normalized = {
        "ops_total": raw["ops_total"],
        "ops_per_sec": round(raw["ops_total"] / elapsed, 1),
        "errors": raw["errors"],
        "elapsed_s": elapsed,
    }
    for key, value in raw.items():
        if key not in ("name", "ts", "elapsed_s", "ops_total", "errors"):
            normalized[key] = value
    return normalized


def compare_module(old_ops, new_ops, module="", gate_enabled=False) -> CompareResult:
    """按阈值比较单模块吞吐，返回 CompareResult。

    - old_ops <= 0（无有效基线）→ NO_COMPARABLE_BASELINE；
    - 退化超过 escalate（默认 20%，CoCond 40%）→ FAIL；
      gate_enabled=False（初期默认）时降级为 WARN，不阻塞 PR；
    - 退化超过 threshold（默认 10%，CoCond 30%）→ WARN；
    - 其余 → PASS。
    """
    old_ops = float(old_ops or 0)
    new_ops = float(new_ops or 0)
    if old_ops <= 0:
        return CompareResult(
            status="NO_COMPARABLE_BASELINE",
            module=module,
            detail={"reason": "baseline_zero_ops"},
        )

    delta_pct = (new_ops - old_ops) / old_ops * 100
    threshold = THRESHOLD_COCOND if module == "cocond" else THRESHOLD_WARN
    escalate = (THRESHOLD_COCOND + 10) if module == "cocond" else THRESHOLD_FAIL

    if delta_pct <= -escalate:
        status = "FAIL" if gate_enabled else "WARN"
    elif delta_pct <= -threshold:
        status = "WARN"
    else:
        status = "PASS"

    return CompareResult(
        status=status,
        module=module,
        old_ops=old_ops,
        new_ops=new_ops,
        delta_pct=round(delta_pct, 3),
    )


def validate_report(report) -> ValidationResult:
    """校验统一报告：顶层字段齐全且 verdict 合法。"""
    if not isinstance(report, dict):
        return ValidationResult(
            status="METRIC_INVALID", missing=tuple(sorted(REQUIRED_TOP_LEVEL))
        )
    missing = tuple(sorted(REQUIRED_TOP_LEVEL - set(report)))
    if missing:
        return ValidationResult(status="METRIC_INVALID", missing=missing)
    if report.get("verdict") not in VALID_VERDICTS:
        return ValidationResult(status="METRIC_INVALID", missing=("verdict",))
    return ValidationResult(status="PASS")


def make_report(*, repository, commit, base_commit, environment, build,
                parameters, modules, verdict) -> dict:
    """构造统一报告 dict（schema_version 固定为当前版本）。"""
    return {
        "schema_version": SCHEMA_VERSION,
        "repository": repository,
        "commit": commit,
        "base_commit": base_commit,
        "environment": environment,
        "build": build,
        "parameters": parameters,
        "modules": modules,
        "verdict": verdict,
    }


# ── 环境指纹采集（唯一副作用来源）──

def _tool_version(cmd, cwd=None) -> str:
    """取工具版本首行；命令不可用或失败时返回空串（指纹字段可为空）。"""
    result = run_command(cmd, cwd=cwd, timeout=10)
    if result.category != "pass":
        return ""
    first = result.stdout.strip().splitlines()
    return first[0].strip() if first else ""


def _cpu_model() -> str:
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor()


def _mem_total_kb() -> str:
    try:
        for line in Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemTotal:"):
                return line.split()[1] + "kB"
    except OSError:
        pass
    return "unknown"


def collect_environment_fingerprint(threads, modules, durations,
                                    build_type="Release", cmake_args=(),
                                    compiler=None) -> dict:
    """采集环境指纹，返回比较键齐全的 dict。

    - agent/node：Jenkins AGENT_NAME / NODE_NAME，缺失时回退主机名；
    - cpu_model / logical_cores / mem_total_kb：/proc 与 os.cpu_count；
    - compiler / cmake_version / ninja_version：版本命令首行（探测失败为空串）。
    """
    node = os.environ.get("NODE_NAME") or os.environ.get("AGENT_NAME") \
        or platform.node()
    agent = os.environ.get("AGENT_NAME") or os.environ.get("NODE_NAME") \
        or platform.node()
    cxx = compiler or os.environ.get("CXX") or "g++"
    return {
        "agent": agent,
        "node": node,
        "cpu_model": _cpu_model(),
        "logical_cores": os.cpu_count() or 0,
        "mem_total_kb": _mem_total_kb(),
        "compiler": _tool_version([cxx, "--version"]),
        "cmake_version": _tool_version(["cmake", "--version"]),
        "ninja_version": _tool_version(["ninja", "--version"]),
        "build_type": build_type,
        "cmake_args": list(cmake_args),
        "threads": threads,
        "modules": list(modules),
        "durations": list(durations),
    }


def write_markdown_report(path, report) -> None:
    """把统一报告写成 Markdown 摘要（含环境指纹与逐模块表格）。"""
    env = report.get("environment", {}) or {}
    lines = [
        "# Performance Report",
        "",
        f"- schema_version: {report.get('schema_version')}",
        f"- repository: {report.get('repository')}",
        f"- commit: {report.get('commit')}",
        f"- base_commit: {report.get('base_commit')}",
        f"- verdict: **{report.get('verdict')}**",
        "",
        "## Environment",
        "",
    ]
    for key, value in env.items():
        lines.append(f"- {key}: {value}")
    lines += [
        "",
        "## Modules",
        "",
        "| Module | Status | ops_total | ops_per_sec | errors | elapsed_s |",
        "|--------|--------|-----------|-------------|--------|-----------|",
    ]
    for name, metrics in (report.get("modules") or {}).items():
        if metrics.get("error"):
            lines.append(f"| {name} | {metrics.get('status', 'ERROR')} "
                         f"| error: {metrics['error']} | | | |")
            continue
        lines.append(
            f"| {name} | {metrics.get('status', 'N/A')} "
            f"| {metrics.get('ops_total', 0)} "
            f"| {metrics.get('ops_per_sec', 0)} "
            f"| {metrics.get('errors', 0)} "
            f"| {metrics.get('elapsed_s', 0)} |"
        )
    lines.append("")
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text("\n".join(lines))
