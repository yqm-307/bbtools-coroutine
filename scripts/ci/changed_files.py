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
  changed_files_json, classifier_status, classification, perf, focused, event_json
  （focused = 必要 focused 测试名正则；空串 = 无直接耦合测试，workflow 不追加 focused 步）

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

# ---- focused 映射表：变更路径前缀 → ctest 测试名正则 -----------------------
# 普通 PR 的「必要 focused」入口：按受影响源码子系统选与之直接耦合的回归集。
# 键按最具体前缀优先匹配；每个条目的每个 pattern 都必须是 ctest 注册集内
# 测试名（<module>.<file>[_<mode>]）的前缀——离线契约测试（test_changed_files.py）
# 断言每个 pattern 能命中注册集（经 `ctest -N -R <pattern>` 展开），同时断言
# bbt/** 全部源码子目录被覆盖。值中元素之间是 OR 关系（与 ctest -R 一致）。
FOCUSED_MAP = {
    # core 基础设施（锁/闩/线程工具；errcode/log/clock 无独立测试，随 core 走）
    "bbt/core/": ("core\\.", "coroutine\\.coroutine$", "coroutine\\.coroutine_exception$"),
    # pollevent：Event/EventLoop/EvThread 及回调注册表生命周期
    "bbt/pollevent/": ("pollevent\\.", "coroutine\\.eventloop_",
                       "coroutine\\.copollevent_state"),
    # coroutine runtime / 栈 / 调度核心
    "bbt/coroutine/detail/": (
        "coroutine\\.coroutine$", "coroutine\\.coroutine_api$",
        "coroutine\\.coroutine_query", "coroutine\\.coroutine_stack$",
        "coroutine\\.multi_size_stack_runtime",
        "coroutine\\.scheduler_", "coroutine\\.cfs_",
        "coroutine\\.ready_wakeup_", "coroutine\\.runtime_oom_",
        "coroutine\\.processer_driver_", "coroutine\\.worker_",
        "coroutine\\.profiler$", "coroutine\\.co_object_identity$",
        "coroutine\\.coroutine_diag$", "coroutine\\.dns_lifecycle$",
        "coroutine\\.reliability$",
    ),
    "bbt/coroutine/coroutine": (  # coroutine.cc/.hpp 伞入口
        "coroutine\\.coroutine$", "coroutine\\.coroutine_api$",
        "coroutine\\.coroutine_exception$", "coroutine\\.coroutine_cancel$",
        "coroutine\\.g_co$", "coroutine\\.async_syntax$",
        "coroutine\\.exception_stop$", "coroutine\\.reliability$",
    ),
    "bbt/coroutine/syntax/": (
        "coroutine\\.async_syntax$", "coroutine\\.g_co$",
        "coroutine\\.macro_wrap$", "coroutine\\.defer$",
    ),
    "bbt/coroutine/sync/Chan": ("coroutine\\.chan$", "coroutine\\.coselect$"),
    "bbt/coroutine/sync/CoSelect": ("coroutine\\.coselect$",),
    "bbt/coroutine/sync/CoMutex": (
        "coroutine\\.comutex$", "coroutine\\.sync_wait_regist_fail$",
    ),
    "bbt/coroutine/sync/CoCond": ("coroutine\\.cond$",),
    "bbt/coroutine/sync/CoRWMutex": (
        "coroutine\\.co_rwmutex$", "coroutine\\.rwlock_guard$",
    ),
    "bbt/coroutine/sync/CoWaiter": (
        "coroutine\\.cowaiter_", "coroutine\\.combined_wait$",
        "coroutine\\.sync_wait_regist_fail$",
        "coroutine\\.diagnostic_wait_oom$",
    ),
    "bbt/coroutine/sync/CoEventValue": (
        "coroutine\\.coeventvalue", "coroutine\\.cowaiter_payload",
    ),
    "bbt/coroutine/sync/WaitTypes": ("coroutine\\.cowaiter_",),
    "bbt/coroutine/sync/CoLockGuard": ("coroutine\\.lockguard$",),
    "bbt/coroutine/sync/": (  # sync 目录兜底（IChan/ICoLock 接口、__TChan 等）
        "coroutine\\.chan$", "coroutine\\.comutex$", "coroutine\\.cond$",
        "coroutine\\.coselect$", "coroutine\\.coevent$",
        "coroutine\\.cowaiter_", "coroutine\\.coeventvalue",
    ),
    "bbt/coroutine/object/": (
        "coroutine\\.co_object_identity$", "coroutine\\.coeventvalue",
    ),
    "bbt/coroutine/pool/": ("coroutine\\.copool$",),
    "bbt/coroutine/io/": ("coroutine\\.io_executor$",),
    "bbt/coroutine/utils/": (
        "coroutine\\.coroutine_diag$", "coroutine\\.crash_diag$",
        "coroutine\\.stringent_debug$",
    ),
    "bbt/coroutine/": (  # coroutine 兜底（CoPollEvent/CoPoller/Hook/Stack 等）
        "coroutine\\.coroutine$", "coroutine\\.poller$", "coroutine\\.hook",
        "coroutine\\.copoller_", "coroutine\\.copollevent_state",
        "coroutine\\.eventloop_", "coroutine\\.io_executor$",
        "coroutine\\.worker_", "coroutine\\.scheduler_",
    ),
}


def _focused_patterns_for_path(path: str) -> tuple | None:
    """单路径 → focused 测试名正则片段 tuple。

    返回值：
    - tuple（可空）：确定的选择集；空 tuple 表示该 code 路径无直接耦合测试。
    - None：路径属 bbt/tests 但未覆盖或形状异常 → 调用方保守回退全量。
    """
    norm = _normalize_perf_path(path)
    if norm.startswith("tests/"):
        # tests/<module>/Test_<x>.cc → ^<module>\.<x 去前缀>(_|$)；
        # 其余 tests/**（dso 辅助 .cc、共享头、非三层路径）→ None 全量兜底。
        parts = norm.split("/")
        if len(parts) == 3 and parts[2].startswith("Test_") and parts[2].endswith(".cc"):
            base = parts[2][len("Test_"):-len(".cc")]
            return (f"^{parts[1]}\\.{base}(_|$)",)
        return None
    if norm.startswith("bbt/"):
        best = max((p for p in FOCUSED_MAP if norm.startswith(p)),
                   key=len, default=None)
        if best is None:
            return None
        return tuple(FOCUSED_MAP[best])
    return ()


def focused_regex(changed_files) -> str:
    """变更集 → focused ctest 测试名正则；无直接耦合测试时返回空串。

    保守口径（宁可多跑不漏跑）：
    - 任一 CMakeLists.txt（含 tests/CMakeLists.txt）：注册集可能变化 → '.'；
    - bbt/** 未被映射表覆盖、tests/** 非 Test_*.cc 形状 → '.'；
    - 其余 code 路径（scripts/.github/examples/benchmarks 等）不扩大选集；
    - 全 doc / 空集 → ''（docs-only 时 build-test 已整体跳过，双保险）。
    """
    if not changed_files:
        return ""
    patterns: list = []
    for path in changed_files:
        if classify_path(path) != "code":
            continue
        if path.rsplit("/", 1)[-1] == "CMakeLists.txt":
            return "."
        hit = _focused_patterns_for_path(path)
        if hit is None:
            return "."
        patterns.extend(hit)
    seen = list(dict.fromkeys(patterns))
    return "|".join(seen)


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
    # focused 选集：分类非 ok（unknown/over-budget）时按「执行全部」给 '.'，
    # 与 classification/perf 的保守回退同口径，不让未知 diff 漏掉回归。
    focused = focused_regex(files) if status == "ok" else "."

    emit("changed_files_json", payload)
    emit("classifier_status", status if status == "ok" else "unknown")
    emit("classification", classification)
    emit("perf", "true" if perf else "false")
    emit("focused", focused)
    emit("event_json", json.dumps(bounded_event(), ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
