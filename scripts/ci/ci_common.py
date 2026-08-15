#!/usr/bin/env python3
"""Jenkins 测试公共契约：进程结果分类、原子 JSON 写入、命令执行、时间戳、
目录初始化和脱敏环境摘要。

后续 Smoke / PR 性能 / Soak Harness 共享本模块。只依赖标准库，
不读取任何密钥环境变量的值（见 sanitized_env_summary 约束）。
"""

import json
import os
import subprocess
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


def run_command(
    cmd: list[str],
    *,
    cwd: Path | None = None,
    timeout: float | None = None,
    env: dict[str, str] | None = None,
) -> tuple[int, str, str]:
    """执行命令，返回 (returncode, stdout, stderr) 三元组。

    超时抛出 subprocess.TimeoutExpired，由调用方捕获后以
    classify_process_result(None, timed_out=True) 归类；env=None 时继承
    当前进程环境。
    """
    proc = subprocess.run(
        cmd,
        cwd=cwd,
        env=env,
        timeout=timeout,
        capture_output=True,
        text=True,
    )
    return proc.returncode, proc.stdout, proc.stderr


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
