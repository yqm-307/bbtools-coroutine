#!/usr/bin/env python3
"""从真实 git diff 计算受限 changed_files 列表，供 framework classify callee 消费。

来源与局限：本文件为**通用 recipe**，逐字复制自已独立审查的 bbtools-infra #50 影子候选
  infra-worktree/scripts/ci/changed_files.py
（sha256 bbe31710e256b9b9464fdbb363a677974b110a4046a3dc1bef3ccd2d9cf9e70c）。
它与仓库/语言无关（只依赖 git），故原样复用；与「分类/结果公共契约」无关——分类/结果
仍只由已发布 framework callee 负责，本仓不复制第二套契约。局限：仅依赖 git 与
GITHUB_OUTPUT 环境；未知 revision/diff 失败/超预算一律保守回退（见下）。

只做一件事：把「真实仓库 diff」归一为仓库相对路径列表 + 有界事件对象，写入
`$GITHUB_OUTPUT`（未设置时写 stdout，便于离线冒烟）。**不实现分类/结果契约**——
分类/计划/结果判定全部由已发布的 framework callee（`scripts/ci/shared`）负责，本仓
不复制第二套公共分类契约。

保守原则（与现役 ci.yml 的 changes job 同口径）：
- PR：三点 diff `origin/<base>...HEAD`；
- push：`before..sha`；首推（before 为空/全零）回退 `HEAD^..HEAD`；
- 任何无法确定的情形（未知 revision、diff 失败、三点 base 缺失）→ changed_files=[] 且
  classifier_status=unknown，令 callee 判定 conservative-全部，绝不擅自判绿；
- 变更集超出受限预算（文件数/字节数上限）时整集回退为空列表（callee → unknown→执行全部），
  **不裁剪成假子集**。

环境输入（由 workflow 注入；离线冒烟可用同名 env 覆盖）：
  EVENT_NAME, BASE_REF, SHA, BEFORE, REF_NAME, GITHUB_OUTPUT
"""
from __future__ import annotations

import json
import os
import subprocess
import sys

MAX_FILES = 3000
MAX_JSON_BYTES = 60000
ZERO_SHA = "0" * 40


def git(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", *args], capture_output=True, text=True, check=False
    )


def valid_commit(rev: str) -> bool:
    return git("rev-parse", "--verify", "--quiet", f"{rev}^{{commit}}").returncode == 0


def compute_range():
    """返回真实 diff 范围表达式；无法确定时返回 None（保守 unknown）。"""
    name = os.environ.get("EVENT_NAME", "")
    base = os.environ.get("BASE_REF", "")
    sha = os.environ.get("SHA", "")
    before = os.environ.get("BEFORE", "")
    if name == "pull_request":
        if not base:
            return None
        return f"origin/{base}...HEAD"
    if not before or before == ZERO_SHA:
        # 新分支首推：改用 HEAD^ 对比；根提交无法对比 → 保守 unknown
        return "HEAD^..HEAD" if valid_commit("HEAD^") else None
    # force-push 或 before 提交被 GC/不可达 → 保守 unknown
    if valid_commit(before) and sha:
        return f"{before}..{sha}"
    return None


def bounded_event() -> dict:
    name = os.environ.get("EVENT_NAME", "")
    if name == "pull_request":
        return {"name": "pull_request", "base_ref": os.environ.get("BASE_REF") or "main"}
    return {"name": "push", "head_ref": os.environ.get("REF_NAME", "")}


def emit(key: str, value: str) -> None:
    line = f"{key}={value}\n"
    path = os.environ.get("GITHUB_OUTPUT")
    if path:
        with open(path, "a", encoding="utf-8") as handle:
            handle.write(line)
    else:
        sys.stdout.write(line)


def main() -> int:
    diff_range = compute_range()
    files: list[str] = []
    status = "unknown"
    if diff_range is not None:
        proc = git("diff", "--name-only", diff_range)
        if proc.returncode == 0:
            files = [line for line in proc.stdout.splitlines() if line]
            status = "ok"

    payload = json.dumps(files, ensure_ascii=False)
    if len(files) > MAX_FILES or len(payload.encode("utf-8")) > MAX_JSON_BYTES:
        # 超预算：整集回退为空 → callee 判 unknown → 执行全部，保守不放绿
        payload = "[]"

    emit("changed_files_json", payload)
    emit("classifier_status", status)
    emit("event_json", json.dumps(bounded_event(), ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
