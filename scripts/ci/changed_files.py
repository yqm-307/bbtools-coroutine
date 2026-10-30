#!/usr/bin/env python3
# 通用 recipe：从真实 git diff 归一受限变更集，并在本仓内完成路径分类与 perf 触发判定。
# 与仓库/语言无关的 diff 部分源自已独立审查的 bbtools-infra #50 影子候选
# scripts/ci/changed_files.py（sha256 bbe31710e256b9b9464fdbb363a677974b110a4046a3dc1bef3ccd2d9cf9e70c）；
# 分类规则与 perf 触发位为本仓内联实现（CI 不再引用共享 classify/result callee 链）。
"""从真实 git diff 计算受限 changed_files 列表，并完成路径分类与 perf 触发判定。

只做三件事：
1. 把「真实仓库 diff」归一为仓库相对路径列表 + 有界事件对象；
2. 按本仓分类规则把变更集判为 docs-only / code / unknown；
3. 按 perf 触发位把「是否影响性能测量」判为 perf=true/false。

分类规则（路径 → doc | code；全集 doc 才判 docs-only，其余一律 code）：
- `CMakeLists.txt`、`.github/workflows/**`、`*.cmake`、`*.py` 一律判 code
  （`CMakeLists.txt` 虽是 `.txt` 后缀，属构建入口；workflows 与 CI 脚本改动可能
  改变构建/测试行为，绝不允许被 markdown 后缀或目录形状带成 docs-only）；
- `docs/` 前缀路径、`.md`/图片/`.txt` 等文档后缀、`LICENSE` 等根目录文书判 doc；
- 混合变更集（任一 code 路径）判 code；
- 变更集为空或 `classifier_status != ok` 判 unknown，保守执行全部。

perf 触发位（仅以下改动为真，与「性能回归检查」测量面一一对应）：
- `bbt/**`：被测运行时源码；
- `benchmarks/**`：被测驱动 `unified_stress` 及其构件；
- perf 脚本集合（`PERF_FILES`）：测量/比较/工具链脚本自身；
- 根 `CMakeLists.txt`：全局编译选项影响测量结果。
其余路径（tests、docs、scripts 其他文件、examples 等）不触发 perf。
目录改名兼容：`benchmark_test/` 已改名为 `benchmarks/`（D6）；历史 diff 中
出现的旧名经 `RENAME_MAP` 折回新名再匹配，触发集不变；`unknown`（diff 失败/
空集/超预算）时 perf 恒为 true，保守执行，绝不擅自免测。

保守原则：
- PR：三点 diff `origin/<base>...HEAD`；
- push：`before..sha`；首推（before 为空/全零）回退 `HEAD^..HEAD`；
- 任何无法确定的情形（未知 revision、diff 失败、三点 base 缺失）→
  changed_files=[]、classifier_status=unknown、classification=unknown、perf=true，
  下游按「执行全部」处理，绝不擅自判绿；
- 变更集超出受限预算（文件数/字节数上限）时整集回退为空列表
  （classification=unknown、perf=true→执行全部），**不裁剪成假子集**。

输出（写入 `$GITHUB_OUTPUT`，未设置时写 stdout，便于离线冒烟）：
  changed_files_json, classifier_status, classification, perf, event_json

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

# ---- 路径分类（本仓内联规则；唯一真源即本文件，配套单测 test_changed_files.py）----
DOC_SUFFIXES = (".md", ".png", ".jpg", ".jpeg", ".svg", ".gif", ".txt")
DOC_PREFIXES = ("docs/",)
DOC_BASENAMES = {"LICENSE", "NOTICE", "COPYING", "CODEOWNERS", "AUTHORS"}
# 后缀或位置像文档、但实际是构建/CI 入口：改它会改变构建或流水线行为，必须按
# code 处理。CMakeLists.txt 判 code 是本规则存在的第一动机（改动构建入口绝不允许
# 被打成 docs-only 放行）。
CODE_BASENAMES = {"CMakeLists.txt"}
CODE_SUFFIXES = (".cmake", ".py")
CODE_PREFIXES = (".github/workflows/",)

# ---- perf 触发位（与「性能回归检查」测量面对应；改名兼容见 RENAME_MAP）----
PERF_PREFIXES = ("bbt/", "benchmarks/")
# perf 脚本集合：测量 driver、判定契约、报告契约、工具链与基线/压测入口。
# 仅列对测量结论有决定权的文件；scripts/ci 下的单测文件经 *.py 已判 code，
# 但不改测量结果，不在此列。
PERF_FILES = {
    "scripts/ci_perf_check.py",      # 测量 driver（workflow 直接调用）
    "scripts/ci/perf_contract.py",   # 阈值/环境指纹/报告 schema
    "scripts/ci/ci_common.py",       # driver 依赖的命令执行与原子写
    "scripts/ci/prepare_boost.sh",   # 工具链复现（指纹字段）
    "scripts/record_baseline.py",    # 基线记录/趋势
    "scripts/run_fatigue.py",        # 疲劳运行器
    "scripts/run_parallel_stress.sh",# 并行压测
}
# 仅根 CMakeLists.txt 是真（全局编译选项）；子目录 CMakeLists 由 code 分类覆盖，
# 不单独计入 perf 触发。
PERF_BASENAMES = {"CMakeLists.txt"}

# 目录改名兼容：旧路径名 → 新名，匹配仍按新名集合执行。
# （benchmark_test→benchmarks 是 D6 的既定改名；其余目录不涉及 perf 触发集。）
RENAME_MAP = (("benchmark_test/", "benchmarks/"),)


def _normalize_perf_path(path: str) -> str:
    """把改名前的旧路径折到新名，供 perf 触发集匹配。"""
    for old, new in RENAME_MAP:
        if path.startswith(old):
            return new + path[len(old):]
    return path


def triggers_perf(path: str) -> bool:
    """单路径是否影响性能测量结论。"""
    path = _normalize_perf_path(path)
    if path.startswith(PERF_PREFIXES):
        return True
    if path in PERF_FILES:
        return True
    # 仅根 CMakeLists.txt（不含子目录同名文件）
    if "/" not in path and path in PERF_BASENAMES:
        return True
    return False


def classify_path(path: str) -> str:
    """单路径分类：doc 或 code。"""
    base = path.rsplit("/", 1)[-1]
    if base in CODE_BASENAMES:
        return "code"
    if path.startswith(CODE_PREFIXES):
        return "code"
    lowered = path.lower()
    if lowered.endswith(CODE_SUFFIXES):
        return "code"
    if path.startswith(DOC_PREFIXES):
        return "doc"
    if lowered.endswith(DOC_SUFFIXES):
        return "doc"
    if base in DOC_BASENAMES:
        return "doc"
    return "code"


def classify_changes(changed_files) -> str:
    """变更集分类：docs-only | code | unknown。"""
    if not changed_files:
        # 无变更数据视为 unknown，保守执行，不擅自判绿。
        return "unknown"
    if all(classify_path(p) == "doc" for p in changed_files):
        return "docs-only"
    return "code"


def classify_status(status: str, changed_files) -> str:
    """结合 classifier 状态：失败/未知一律 conservative unknown。"""
    if status != "ok":
        return "unknown"
    return classify_changes(changed_files)


def perf_status(status: str, changed_files) -> bool:
    """perf 触发位：非 ok 状态或空集恒真（保守执行）；ok 时按路径判定。"""
    if status != "ok" or not changed_files:
        return True
    return any(triggers_perf(p) for p in changed_files)


# ---- 真实 diff 归一 ----
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
        # 超预算：整集回退为空 → unknown + perf=true → 执行全部，保守不放绿
        payload = "[]"
        status = "over-budget"

    classification = classify_status(status, files if status == "ok" else [])
    perf = perf_status(status, files if status == "ok" else [])

    emit("changed_files_json", payload)
    emit("classifier_status", status if status == "ok" else "unknown")
    emit("classification", classification)
    emit("perf", "true" if perf else "false")
    emit("event_json", json.dumps(bounded_event(), ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
