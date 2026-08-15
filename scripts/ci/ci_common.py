#!/usr/bin/env python3
"""Jenkins 测试公共契约：进程结果分类、原子 JSON 写入、命令执行、时间戳、
目录初始化和脱敏环境摘要。

后续 Smoke / PR 性能 / Soak Harness 共享本模块。只依赖标准库，
不读取任何密钥环境变量的值（见 sanitized_env_summary 约束）。
"""

import json
import os
import re
import subprocess
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

# 密钥环境变量判定关键字：键名命中任一关键字即脱敏，且绝不读取其值。
# 判定偏保守（宁可多脱敏不可泄漏），如 MONKEY 这类误命中只会隐藏非敏感值。
_SECRET_KEYWORDS = ("TOKEN", "SECRET", "PASSWORD", "PASSWD", "AUTH", "KEY")


def classify_process_result(returncode: int | None, timed_out: bool) -> str:
    """把进程退出信息归类为稳定字符串，供 Jenkins 阶段分类与报告使用。

    分类优先级：timeout > unknown_exit > crash > failure > pass。
    - timed_out=True 时忽略 returncode，统一为 "timeout"；
    - returncode=None 表示进程未正常返回（如被主动终止），记为 "unknown_exit"；
    - returncode < 0 为被信号杀死（-11 即 SIGSEGV），记为 "crash"；
    - returncode != 0 为普通失败，记为 "failure"；
    - 其余为 "pass"。
    """
    if timed_out:
        return "timeout"
    if returncode is None:
        return "unknown_exit"
    if returncode < 0:
        return "crash"
    if returncode != 0:
        return "failure"
    return "pass"


@dataclass(frozen=True)
class CommandResult:
    """命令执行结果：退出信息、输出与统一分类。

    category 由 classify_process_result 得出（pass/failure/crash/timeout/
    unknown_exit），timed_out 标记是否超时终止，duration 为墙钟耗时秒数。
    """

    returncode: int | None
    stdout: str
    stderr: str
    category: str
    timed_out: bool
    duration: float


def run_command(
    cmd: list[str],
    *,
    cwd: Path | None = None,
    timeout: float | None = None,
    env: dict[str, str] | None = None,
    sigterm_grace: float = 5.0,
) -> CommandResult:
    """执行命令并返回 CommandResult；env=None 时继承当前进程环境。

    超时不抛异常：先向进程发送 SIGTERM，等待 sigterm_grace 秒后若仍存活
    再发送 SIGKILL，随后统一归类为 "timeout"。timeout=None 时无限等待。
    返回结果含 stdout/stderr 全文，供调用方扫描错误模式或写入日志。
    """
    start = time.monotonic()
    proc = subprocess.Popen(
        cmd,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    timed_out = False
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        proc.terminate()
        try:
            proc.wait(timeout=sigterm_grace)
        except subprocess.TimeoutExpired:
            proc.kill()
        stdout, stderr = proc.communicate()
    duration = time.monotonic() - start
    return CommandResult(
        returncode=proc.returncode,
        stdout=stdout,
        stderr=stderr,
        category=classify_process_result(proc.returncode, timed_out),
        timed_out=timed_out,
        duration=duration,
    )


# dump 文件判定：Linux core 文件（core / core.<pid> / core.<exe>.<pid>，
# 后者覆盖 core_pattern=core.%e.%p 等常见自定义配置，如 core.app.12345）
# 与常见 dump 扩展名。仅按文件名判定，不读内容；名字含 "core" 的普通文件
# （如 core_docs.md、core.lib）不算，避免误报。core.%p.%e 这类 pid 在前的
# 自定义顺序不在覆盖范围，注释记录该局限。
_CORE_RE = re.compile(r"^core(\.\d+)?(\.[^.]+\.\d+)?$")
_DUMP_SUFFIXES = (".dump", ".dmp", ".mdmp")


def _is_dump_file(path: Path) -> bool:
    name = path.name
    if _CORE_RE.match(name):
        return True
    return path.suffix.lower() in _DUMP_SUFFIXES


def find_dump_files(dirs: list[Path]) -> list[Path]:
    """递归扫描 dirs，返回 core/dump 文件路径列表（跳过 .git 目录，排序去重）。"""
    hits: set[Path] = set()
    for root in dirs:
        if not root.exists():
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d != ".git"]
            for filename in filenames:
                candidate = Path(dirpath) / filename
                if _is_dump_file(candidate):
                    hits.add(candidate)
    return sorted(hits)


def write_json_atomic(path: Path, value: object) -> None:
    """把 value 原子写入 path：先写同目录 .tmp 文件再替换，避免半截 JSON。

    自动创建父目录；文件内容为带缩进、保留非 ASCII 字符的 JSON 加换行。
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")
    temporary.replace(path)


def utc_timestamp() -> str:
    """当前 UTC 时间的 ISO 8601 字符串（含时区、秒精度），用于报告时间戳。"""
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def ensure_dir(path: Path) -> Path:
    """创建目录（含父目录），幂等，返回传入路径。"""
    path.mkdir(parents=True, exist_ok=True)
    return path


def sanitized_env_summary(env: dict[str, str] | None = None) -> dict[str, str]:
    """返回环境摘要：非密钥变量带值，密钥变量只列键名、值统一为 "<redacted>"。

    密钥判定按 _SECRET_KEYWORDS 子串匹配键名（不区分大小写）；命中时
    实现上不访问 env[key]，从源头避免把真实密钥值带入日志或报告。
    """
    source = os.environ if env is None else env
    summary: dict[str, str] = {}
    for key in source:
        upper = key.upper()
        if any(keyword in upper for keyword in _SECRET_KEYWORDS):
            summary[key] = "<redacted>"
        else:
            summary[key] = source[key]
    return summary
