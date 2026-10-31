#!/usr/bin/env python3
"""bbtools-coroutine 正式 `.github/workflows/ci.yml`（唯一权威普通入口）离线冒烟与契约测试。

本文件原名 `test_shadow_ci.py`（影子身份）。hosted shadow Layer1 先升为正式普通门禁
（旧 `unit_test.yml`），现 workflow 改名并收敛为单文件 `ci.yml`：docs-check.yml 全部检查
（空白 diff / 密钥扫描 / check_docs.py）并入 `docs-check` job，framework classify/result
callee 链删除（D3），分类由 `changed_files.py` 内联、结果判定由 `result.py` 内联，
perf-regression 改按 `changes` 的 perf 输出位触发（D4）。本文件随之改写为对**实际正式
Recipe** 的不可少守卫（构建判据 / retry 上界 / 工具自测调用入口 / Boost 前缀 /
release_gate 消费者契约 / result.py required-optional 语义 / docs-check 属性与
fail-closed 行为）。

会被正式 `ci.yml` 的 `python3 -m unittest discover -s scripts/ci -p 'test_*.py'`
**自动发现**，无需新增 run_tests.py 入口（避免碰既有测试入口）。本地只验证「CI 配方 /
workflow 静态契约 / docs-check 行为」，**不跑 C++ 全量构建**（真实构建走父级授权的 PR CI）。

覆盖：
- 配方守卫：脚本 bash -n、参数校验、fail-closed 拒绝已存在前缀 / 未知 revision / 非法 jobs；
- changed_files 路由：push/PR 真实 diff 归一、未知保守回退、超预算整集回退
  （分类/perf 触发位用例集在 `test_changed_files.py`）；
- 正式 workflow 文本契约（stdlib，恒跑，直接读真实 ci.yml）：触发面 / 权限 / SHA pin /
  hosted runs-on / concurrency / 无 callee / required-optional 内联 / docs-only 门控 /
  always 汇聚 / 无 artifact / 无 cache / 性能 job 为同 runner base/head 短测（只构建
  unified_stress，不跑全量 CTest）/ 无 sanitizer 长测 / 影子已退役；
- 正式 workflow 结构契约（需 PyYAML，缺省显式 SKIP，不等于通过）；
- 正式 Recipe 不可少守卫（含 release_gate 消费者契约）；
- 结果判定端到端：直接执行本仓 scripts/ci/result.py CLI（required/optional/docs-only
  放行语义与 result job heredoc 同一 payload 形状）；
- docs-check：文本契约（恒跑 job / SHA pin / 无表达式注入 / 无网络链接检查工具）与
  行为负例（SHA 格式非法 fail-closed、真实空白错误可抓、空白范围干净放行、密钥扫描
  rc>1 fail-closed、synthetic token 命中）。

运行：
  PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s scripts/ci -p 'test_*.py'
  # 需要 PyYAML 才跑结构契约，否则显式 SKIP。
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
WORKTREE = os.path.normpath(os.path.join(HERE, "..", ".."))
WORKFLOWS = os.path.join(WORKTREE, ".github", "workflows")
CI_YML = os.path.join(WORKFLOWS, "ci.yml")
# 影子资产（本候选已授权退役）：入口与说明都必须不存在。
SHADOW_YML = os.path.join(WORKFLOWS, "ci-shadow-v1.yml")
SHADOW_DOC = os.path.join(WORKTREE, "agent-docs", "ci-shadow-v1.md")
FORMAL_DOC = os.path.join(WORKTREE, "docs", "guides", "ci.md")
SHADOW_TEST = os.path.join(HERE, "test_shadow_ci.py")
RETIRED_YMLS = tuple(
    os.path.join(WORKFLOWS, name)
    for name in ("unit_test.yml", "docs-check.yml", "memery_test_info.yml")
)
RELEASE_GATE = os.path.join(WORKTREE, "scripts", "release_gate.py")
PREPARE_BOOST = os.path.join(HERE, "prepare_boost.sh")
CHANGED = os.path.join(HERE, "changed_files.py")
RESULT = os.path.join(HERE, "result.py")

CHECKOUT_SHA = "11d5960a326750d5838078e36cf38b85af677262"
SHA_PIN_RE = re.compile(r"^[^@\s]+@[0-9a-f]{40}$")

REQUIRED = ["changes", "docs-check"]
OPTIONAL = ["build-test", "perf-regression"]
HEAVY = ("build-test", "perf-regression")
BUILD_CHECK_NAME = "编译 & 单元测试"
PERF_CHECK_NAME = "性能回归检查"
DOCS_CHECK_NAME = "文档检查（空格/链接/密钥）"

ENV = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")


def raw(path: str) -> str:
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def body(path: str) -> str:
    """去掉说明性注释行后的实际 YAML 内容（注释中可提及被禁写法）。"""
    return "\n".join(
        line for line in raw(path).splitlines() if not line.lstrip().startswith("#")
    )


def gha_format(fmt, *args):
    """模拟 GitHub Actions format()：`{{`/`}}` 为字面花括号，`{N}` 取第 N 个参数。"""
    out, i = [], 0
    while i < len(fmt):
        ch = fmt[i]
        if ch == "{":
            if i + 1 < len(fmt) and fmt[i + 1] == "{":
                out.append("{")
                i += 2
                continue
            end = fmt.index("}", i)
            out.append(str(args[int(fmt[i + 1:end])]))
            i = end + 1
            continue
        if ch == "}":
            if i + 1 < len(fmt) and fmt[i + 1] == "}":
                out.append("}")
                i += 2
                continue
            raise ValueError("unbalanced '}' in format template")
        out.append(ch)
        i += 1
    return "".join(out)


def format_template(fmt_source, needle):
    """从文本里取含 needle 行中 format('...') 的单引号模板（模板内不含单引号）。"""
    for line in fmt_source.splitlines():
        if needle in line and "format(" in line:
            match = re.search(r"format\('([^']*)'", line)
            if match:
                return match.group(1)
    raise AssertionError(f"未找到含 {needle!r} 的 format 模板")


def run(cmd, **kwargs):
    return subprocess.run(cmd, capture_output=True, text=True, check=False, **kwargs)


def _extract_run_blocks(text: str):
    """依赖无关地按缩进提取每个 `run: |` 块的实际脚本内容（保留原缩进）。"""
    blocks = []
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        match = re.match(r"^(\s*)run:\s*\|?\s*$", lines[i])
        if not match:
            i += 1
            continue
        indent = len(match.group(1))
        i += 1
        buf = []
        while i < len(lines):
            line = lines[i]
            if line.strip() and (len(line) - len(line.lstrip())) <= indent:
                break
            buf.append(line)
            i += 1
        blocks.append("\n".join(buf))
    return blocks


def _job_block(stripped: str, job_id: str, following: str) -> str:
    """取去注释正文中 `  <job_id>:` 到下一个顶层 job 键之间的块。"""
    return stripped.split(f"  {job_id}:", 1)[1].split(f"\n  {following}:", 1)[0]


def _block_containing(path: str, needle: str) -> str:
    for block in _extract_run_blocks(raw(path)):
        if needle in block:
            return block
    raise AssertionError(f"未在 {os.path.basename(path)} 找到含 {needle!r} 的 run 块")


# --------------------------------------------------------------------------- #
# 配方守卫
# --------------------------------------------------------------------------- #
class RecipeGuardTests(unittest.TestCase):
    def test_shell_recipes_parse(self):
        proc = run(["bash", "-n", PREPARE_BOOST], env=ENV)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_python_recipes_compile(self):
        # 用 compile() 做纯语法检查，不落 .pyc（PYTHONDONTWRITEBYTECODE 对 py_compile 无效）。
        for path in (CHANGED, RESULT, os.path.abspath(__file__)):
            with self.subTest(path=os.path.basename(path)):
                compile(raw(path), path, "exec")

    def test_prepare_boost_print_plan_is_side_effect_free_and_pinned(self):
        proc = run(["bash", PREPARE_BOOST, "--print-plan"], env=ENV)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        plan = dict(
            line.split("=", 1) for line in proc.stdout.splitlines() if "=" in line
        )
        self.assertEqual(plan["boost_version"], "1.90.0")
        self.assertEqual(
            plan["boost_sha256"],
            "5e93d582aff26868d581a52ae78c7d8edf3f3064742c6e77901a1f18a437eea9",
        )
        self.assertEqual(plan["boost_libs"], "context")
        self.assertEqual(plan["boost_version_num"], "109000")
        self.assertEqual(
            plan["archive_url"],
            "https://archives.boost.io/release/1.90.0/source/boost_1_90_0.tar.gz",
        )

    def test_prepare_boost_refuses_existing_prefix(self):
        with tempfile.TemporaryDirectory() as td:
            existing = os.path.join(td, "prefix")
            os.makedirs(existing)
            proc = run(["bash", PREPARE_BOOST, existing], env=ENV)
            self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
            self.assertTrue(os.path.isdir(existing), "拒绝时不得改动已存在前缀")

    def test_prepare_boost_refuses_dangling_symlink(self):
        with tempfile.TemporaryDirectory() as td:
            link = os.path.join(td, "dangling")
            os.symlink(os.path.join(td, "never-created"), link)
            proc = run(["bash", PREPARE_BOOST, link], env=ENV)
            self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
            self.assertTrue(os.path.islink(link))

    def test_prepare_boost_rejects_missing_prefix(self):
        proc = run(["bash", PREPARE_BOOST], env=ENV)
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)

    def test_prepare_boost_rejects_invalid_jobs(self):
        with tempfile.TemporaryDirectory() as td:
            proc = run(
                ["bash", PREPARE_BOOST, os.path.join(td, "p"), "--jobs", "x"], env=ENV
            )
            self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)

    def test_prepare_boost_rejects_unknown_option(self):
        with tempfile.TemporaryDirectory() as td:
            proc = run(
                ["bash", PREPARE_BOOST, os.path.join(td, "p"), "--nope"], env=ENV
            )
            self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)


# --------------------------------------------------------------------------- #
# 构建入口 ↔ 子目录一致性守卫（回归：debug/ 曾被删除而 NEED_DEBUG 入口仍注册，
# NEED_DEBUG=ON configure 直接失败——目录与 CMake 入口必须同生同灭）
# --------------------------------------------------------------------------- #
ROOT_CMAKELISTS = os.path.join(WORKTREE, "CMakeLists.txt")


def _gated_subdir_block(root_text: str, option: str) -> str:
    """取根 CMakeLists 中 `if (<OPTION>) ... endif()` 的实际块文本（不允许入口漂出守卫）。

    按行扫描并跟踪 if/endif 嵌套深度：守卫块内部允许再嵌套条件
    （如 fail-closed 的 NOT EXISTS 检查），只在深度回到 0 的 endif 处收尾。
    """
    lines = root_text.splitlines()
    start = None
    depth = 0
    for i, line in enumerate(lines):
        stripped = line.strip()
        if re.match(rf"^if\s*\(\s*{re.escape(option)}\s*\)", stripped) and start is None:
            start = i
            depth = 1
            continue
        if start is None:
            continue
        if re.match(r"^if\s*\(", stripped):
            depth += 1
        elif re.match(r"^endif\s*\(", stripped) or stripped == "endif()":
            depth -= 1
            if depth == 0:
                return "\n".join(lines[start + 1:i])
    raise AssertionError(f"根 CMakeLists.txt 缺少 if ({option}) 块")


class NeedDebugWiringContractTests(unittest.TestCase):
    """NEED_DEBUG 入口与 debug/ 目录的一致性：任一缺失即 NEED_DEBUG=ON 配置失败。"""

    @classmethod
    def setUpClass(cls):
        cls.root = raw(ROOT_CMAKELISTS)
        cls.debug_dir = os.path.join(WORKTREE, "debug")
        cls.debug_cmake = os.path.join(cls.debug_dir, "CMakeLists.txt")

    def test_option_and_gated_subdirectory_both_registered(self):
        self.assertIn("option(NEED_DEBUG", self.root, "NEED_DEBUG option 必须存在")
        block = _gated_subdir_block(self.root, "NEED_DEBUG")
        self.assertIn('add_subdirectory_with_build_type(debug "Debug")', block,
                      "debug 子目录必须注册在 NEED_DEBUG 守卫内，不得漂出或删除")

    def test_needs_debug_guard_fails_closed_before_add_subdirectory(self):
        # 回归契约：NEED_DEBUG=ON 且 debug/CMakeLists.txt 缺失时必须以明确的
        # FATAL_ERROR 在 add_subdirectory 之前终止，而不是靠 CMake 默认报错碰运气。
        block = _gated_subdir_block(self.root, "NEED_DEBUG")
        self.assertIn('EXISTS "${PROJECT_SOURCE_DIR}/debug/CMakeLists.txt"', block,
                      "NEED_DEBUG 块必须先显式检查 debug/CMakeLists.txt 存在")
        self.assertIn("FATAL_ERROR", block,
                      "debug/CMakeLists.txt 缺失时必须 message(FATAL_ERROR ...) 终止")
        self.assertLess(block.index("FATAL_ERROR"),
                        block.index('add_subdirectory_with_build_type(debug "Debug")'),
                        "fail-closed 检查必须位于 add_subdirectory 之前")

    def test_debug_dir_and_its_cmakelists_exist(self):
        self.assertTrue(os.path.isdir(self.debug_dir),
                        "根 CMakeLists 注册了 debug 子目录，debug/ 不得缺失")
        self.assertTrue(os.path.isfile(self.debug_cmake),
                        "debug/CMakeLists.txt 必须存在，否则 NEED_DEBUG=ON configure 失败")

    def test_every_declared_debug_source_exists(self):
        # debug/CMakeLists.txt 里每个 add_executable(<名> <源>.cc) 的源文件必须在盘上；
        # 历史上整目录被删时本断言与 dir 存在性一起失败，源文件单删时单独失败。
        text = raw(self.debug_cmake)
        declared = re.findall(
            r"add_executable\s*\(\s*\S+\s+(\S+\.cc)\s*\)", text
        )
        self.assertTrue(declared, "debug/CMakeLists.txt 至少声明一个调试可执行目标")
        for source in declared:
            with self.subTest(source=source):
                self.assertTrue(
                    os.path.isfile(os.path.join(self.debug_dir, source)),
                    f"debug/{source} 被 add_executable 引用但文件缺失",
                )


# --------------------------------------------------------------------------- #
# changed_files 路由
# --------------------------------------------------------------------------- #
def _git(repo, *args):
    return run(["git", "-C", repo, *args], env=ENV)


def _init_repo(repo):
    _git(repo, "init", "-q", "-b", "main")
    _git(repo, "config", "user.email", "formal@test")
    _git(repo, "config", "user.name", "formal")
    _git(repo, "config", "commit.gpgsign", "false")


def _commit(repo, relpath, content, message):
    full = os.path.join(repo, relpath)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w", encoding="utf-8") as handle:
        handle.write(content)
    _git(repo, "add", relpath)
    _git(repo, "commit", "-q", "-m", message)
    return _git(repo, "rev-parse", "HEAD").stdout.strip()


def _changed_files(repo, env_extra):
    out_path = os.path.join(repo, ".formal-output")
    env = dict(ENV, GITHUB_OUTPUT=out_path, **env_extra)
    proc = run([sys.executable, CHANGED], env=env, cwd=repo)
    with open(out_path, encoding="utf-8") as handle:
        values = dict(
            line.split("=", 1) for line in handle.read().splitlines() if "=" in line
        )
    return proc, values


class ChangedFilesRoutingTests(unittest.TestCase):
    def test_push_range_yields_real_paths(self):
        with tempfile.TemporaryDirectory() as repo:
            _init_repo(repo)
            first = _commit(repo, "docs/a.md", "a", "docs")
            second = _commit(repo, "bbt/coroutine/x.cc", "x", "code")
            proc, values = _changed_files(
                repo,
                {"EVENT_NAME": "push", "BEFORE": first, "SHA": second,
                 "REF_NAME": "main"},
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(values["classifier_status"], "ok")
            self.assertEqual(json.loads(values["changed_files_json"]), ["bbt/coroutine/x.cc"])
            self.assertEqual(
                json.loads(values["event_json"]),
                {"name": "push", "head_ref": "main"},
            )

    def test_pull_request_three_dot(self):
        with tempfile.TemporaryDirectory() as repo:
            _init_repo(repo)
            base = _commit(repo, "docs/a.md", "a", "base")
            _git(repo, "update-ref", "refs/remotes/origin/main", base)
            _git(repo, "checkout", "-q", "-b", "feat")
            head = _commit(repo, "docs/guides/ci.md", "n", "note")
            proc, values = _changed_files(
                repo,
                {"EVENT_NAME": "pull_request", "BASE_REF": "main", "SHA": head},
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(
                json.loads(values["changed_files_json"]), ["docs/guides/ci.md"]
            )
            self.assertEqual(
                json.loads(values["event_json"]),
                {"name": "pull_request", "base_ref": "main"},
            )

    def test_unknown_range_falls_back_conservative(self):
        with tempfile.TemporaryDirectory() as repo:
            _init_repo(repo)
            first = _commit(repo, "bbt/x.cc", "x", "one")
            proc, values = _changed_files(
                repo,
                {"EVENT_NAME": "push", "BEFORE": "f" * 40, "SHA": first},
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(values["changed_files_json"], "[]")
            self.assertEqual(values["classifier_status"], "unknown")

    def test_oversized_change_set_falls_back_to_empty(self):
        with tempfile.TemporaryDirectory() as repo:
            _init_repo(repo)
            first = _commit(repo, "bbt/gen0.cc", "0", "base")
            for i in range(1, 6):
                _commit(repo, f"bbt/gen{i}.cc", "x", f"c{i}")
            head = _git(repo, "rev-parse", "HEAD").stdout.strip()
            # 人为把文件数上限调低到 2（避免造上千文件），验证超预算整集回退为空。
            out_path = os.path.join(repo, ".o2")
            env = dict(ENV, GITHUB_OUTPUT=out_path, EVENT_NAME="push",
                       BEFORE=first, SHA=head)
            script = (
                "import sys; sys.path.insert(0, %r); "
                "import changed_files as m; m.MAX_FILES = 2; sys.exit(m.main())" % HERE
            )
            proc = run([sys.executable, "-c", script], env=env, cwd=repo)
            with open(out_path, encoding="utf-8") as handle:
                values = dict(
                    line.split("=", 1)
                    for line in handle.read().splitlines() if "=" in line
                )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(values["changed_files_json"], "[]")


# --------------------------------------------------------------------------- #
# 正式 workflow 文本契约（stdlib，恒跑，直接读真实文件）
# --------------------------------------------------------------------------- #
class FormalWorkflowTextContractTests(unittest.TestCase):
    """不依赖 PyYAML 的静态契约：直接读真实 .github/workflows/ci.yml 文本。"""

    @classmethod
    def setUpClass(cls):
        cls.text = raw(CI_YML)
        cls.stripped = body(CI_YML)

    def test_name_and_identity(self):
        self.assertIn("\nname: CI\n", self.text, "workflow 名必须保持 CI")
        # path 即本文件自身：release_gate.validate_main_ci 按 workflows/ci.yml 读取。
        self.assertTrue(os.path.isfile(CI_YML))
        self.assertEqual(os.path.basename(CI_YML), "ci.yml")

    def test_trigger_is_main_pr_push_and_dispatch_only(self):
        self.assertEqual(self.stripped.count("branches: [main]"), 2,
                         "pull_request 与 push 各保留 branches: [main]")
        self.assertIn("workflow_dispatch:", self.stripped,
                      "手动重跑普通 build→test 入口")
        for forbidden in ("schedule:", "merge_group", "ci/issue-50-hosted-shadow"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.stripped)

    def test_retired_shadow_and_split_workflows_are_gone_no_double_run(self):
        for path in (SHADOW_YML, SHADOW_DOC, SHADOW_TEST, *RETIRED_YMLS):
            with self.subTest(path=os.path.basename(path)):
                self.assertFalse(os.path.exists(path),
                                 "影子资产与旧拆分 workflow 已授权退役/改名，不得残留")
        self.assertTrue(os.path.isfile(FORMAL_DOC), "正式说明必须存在")
        self.assertTrue(os.path.isfile(os.path.abspath(__file__)))
        # 未被授权改动的邻居 workflow 必须仍在。
        for keep in ("memcheck.yml", "release.yml"):
            with self.subTest(keep=keep):
                self.assertTrue(os.path.isfile(os.path.join(WORKFLOWS, keep)))

    def test_permissions_minimal_no_write_no_secrets(self):
        self.assertIn("permissions: {}", self.stripped)
        self.assertNotIn("secrets: inherit", self.text)
        self.assertNotIn("${{ secrets.", self.text)
        self.assertNotRegex(self.text, r"(?m)^\s*secrets:\s*$")
        self.assertNotIn("id-token", self.text)
        self.assertNotRegex(self.stripped, r"(?m)^\s*[a-z-]+:\s*write\s*$")
        self.assertIn("persist-credentials: false", self.text)

    def test_concurrency_per_run_main_pr_only_cancel(self):
        self.assertIn("CI-${{", self.stripped)
        self.assertIn("github.run_id", self.stripped)
        self.assertIn("cancel-in-progress: ${{ github.event_name == 'pull_request' }}",
                      self.stripped)

    def test_local_jobs_are_hosted_ubuntu_24_04(self):
        runs_on = re.findall(r"(?m)^\s*runs-on:\s*(\S+)\s*$", self.stripped)
        self.assertTrue(runs_on, "应存在本地 job 的 runs-on")
        self.assertEqual(set(runs_on), {"ubuntu-24.04"})
        for forbidden in ("arc-s4", "self-hosted"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.stripped)

    def test_all_uses_are_full_sha_pinned(self):
        for uses in re.findall(r"(?m)^\s*uses:\s*(\S+)\s*$", self.stripped):
            with self.subTest(uses=uses):
                self.assertRegex(uses, SHA_PIN_RE)
        self.assertIn(f"actions/checkout@{CHECKOUT_SHA}", self.stripped)

    def test_no_reusable_callee_or_local_workflow_call(self):
        # classify/result callee 链已删除（D3）：无 workflow_call 触发器，无
        # `owner/repo/.github/workflows/...@sha` 或 `./.github/workflows` 复用。
        self.assertNotIn("workflow_call", self.stripped)
        self.assertNotIn("/.github/workflows/", self.stripped)
        self.assertNotIn("./.github/workflows", self.stripped)
        self.assertNotIn("bbtools-classify", self.stripped)
        for forbidden in ("required_checks_json:", "optional_checks_json:",
                          "classifier_status:"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.stripped)

    def test_inline_classification_wiring(self):
        # 分类在本仓 changes job 内联执行（changed_files.py），不经 callee 传参。
        self.assertIn("python3 scripts/ci/changed_files.py", self.stripped)
        self.assertIn("steps.diff.outputs.classification", self.stripped)
        self.assertIn("steps.diff.outputs.perf", self.stripped)

    def test_build_job_keeps_required_check_name_as_sole_producer(self):
        # 旧 required context `编译 & 单元测试` 必须恰有一个真实 producer，且不靠空成功 alias 伪造。
        self.assertEqual(self.stripped.count(f'name: "{BUILD_CHECK_NAME}"'), 1)
        # `性能回归检查` 恰有一个 producer：同 runner base/head 真实测量（不伪造 PASS）。
        self.assertEqual(self.stripped.count(f'name: "{PERF_CHECK_NAME}"'), 1)
        # docs-check 迁入本文件后仍恰有一个 producer，display name 不变（context 不悬空）。
        self.assertEqual(self.stripped.count(f'name: "{DOCS_CHECK_NAME}"'), 1)
        self.assertIn("ci_perf_check.py", self.stripped)
        # build-test 唯一 continue-on-error 在 changes job 的 format-diff（告警不阻断，D5）。
        self.assertEqual(self.stripped.count("continue-on-error"), 1,
                         "仅 format-diff 允许告警不阻断；不得伪造门禁 job 结果")

    def test_required_and_optional_disjoint_and_build_test_optional(self):
        # result job 内联 result.py：required/optional 集合写死在 heredoc payload。
        self.assertIn('"required": ["changes", "docs-check"]', self.stripped)
        self.assertIn('"optional": ["build-test", "perf-regression"]', self.stripped)
        # 预期跳过由调用方按触发条件显式声明（expected_skips），result.py 不再
        # 消费 classification：build-test 仅 docs-only、perf-regression 仅
        # perf=false 或 workflow_dispatch 允许 skipped。
        self.assertIn('"expected_skips": skips', self.stripped)
        self.assertIn('CLASSIFICATION"] == "docs-only"', self.stripped)
        self.assertIn('PERF"] == "false"', self.stripped)
        # 重型 build-test 为 optional：仅 docs-only 允许显式跳过，且必须等 changes 成功。
        self.assertIn("needs.changes.outputs.classification != 'docs-only'",
                      self.stripped)
        self.assertIn("needs.changes.result == 'success'", self.stripped)
        self.assertNotIn("docs-only 变更直接通过", self.stripped,
                         "docs-only 走显式跳过，不用空成功 echo 伪造 job 结果")

    def test_result_job_aggregates_always_and_passes_all_job_results(self):
        self.assertIn("if: ${{ always() }}", self.stripped)
        self.assertIn("needs: [changes, docs-check, build-test, perf-regression]",
                      self.stripped)
        self.assertIn(
            "format('{{\"changes\":\"{0}\",\"docs-check\":\"{1}\",\"build-test\":\"{2}\",\"perf-regression\":\"{3}\"}}'",
            self.stripped,
        )
        for ref in ("needs.changes.result", "needs.docs-check.result",
                    "needs.build-test.result", "needs.perf-regression.result"):
            with self.subTest(ref=ref):
                self.assertIn(ref, self.stripped)
        # 汇聚走本仓 result.py（fail-closed，非零即红），不再经 callee。
        self.assertIn("python3 scripts/ci/result.py --file \"$payload\"", self.stripped)

    def test_result_job_has_conservative_fallbacks(self):
        # changes 失败时 outputs 为空 → classification/perf 回退 unknown/'true'
        # → 不声明任何预期跳过 → 执行全部。
        self.assertIn("|| 'unknown'", self.stripped)
        self.assertIn(
            "CLASSIFICATION: ${{ needs.changes.outputs.classification || 'unknown' }}",
            self.stripped,
        )
        self.assertIn(
            "PERF: ${{ needs.changes.outputs.perf || 'true' }}",
            self.stripped,
        )

    def test_no_perf_release_memcheck_artifact_or_cache(self):
        # 发布 / memcheck / artifact / cache 都不在本正式候选内。性能 job 现在**允许**在 hosted
        # 真实构建 unified_stress 并跑 ci_perf_check（同 runner base/head 短测），但**仍**禁止：
        # 正式基线工作树（tests/baselines）、基线记录（record_baseline）、发布（release_gate.py
        # publish）、memcheck、artifact、cache——这些会改变正式基线/产物/缓存语义。
        for forbidden in (
            "tests/baselines", "run_memcheck", "record_baseline",
            "release_gate.py publish", "upload-artifact", "download-artifact",
            "actions/cache", "ccache",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.stripped)
        # 同 runner 短测必须真的接线（不是被悄悄删回空成功）。
        self.assertIn("unified_stress", self.stripped)
        self.assertIn("ci_perf_check.py", self.stripped)
        self.assertIn("prepare_boost.sh", self.stripped)

    def test_no_sanitizer_long_run_trigger(self):
        # sanitizer 旧 job 实际不可达（旧 on 无 workflow_dispatch）；本轮不开启新长期执行。
        for forbidden in ("sanitizer", "Sanitizer", "ASAN", "build_sanitizer.sh"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.stripped)

    def test_preserves_gate_commands(self):
        for needle in (
            "-DCMAKE_BUILD_TYPE=Release -DNEED_TEST=ON -DNEED_BENCHMARK=ON",
            'cmake --build . --parallel "$JOBS"',
            "ctest --timeout 60 --output-on-failure",
            "for bin in 90s:Test_smoke 120s:Test_reliability",
            '"bin/tests/$name" --log_level=test_suite',
            'bash scripts/ci/prepare_boost.sh "$BOOST_PREFIX"',
            'python3 scripts/test_release_gate.py',
            "python3 -m unittest discover -s scripts/ci -p 'test_*.py'",
        ):
            with self.subTest(needle=needle):
                self.assertIn(needle, self.stripped)

    def test_inline_scripts_pass_bash_n(self):
        blocks = _extract_run_blocks(self.text)
        self.assertTrue(blocks, "应提取到至少一个 run 块")
        for script in blocks:
            with self.subTest(head=script.strip().splitlines()[0] if script.strip() else ""):
                fd, tmp = tempfile.mkstemp(suffix=".sh")
                try:
                    with os.fdopen(fd, "w", encoding="utf-8") as handle:
                        handle.write(script)
                    proc = run(["bash", "-n", tmp], env=ENV)
                    self.assertEqual(proc.returncode, 0, proc.stderr + script)
                finally:
                    os.unlink(tmp)


# --------------------------------------------------------------------------- #
# 正式 workflow 结构契约（需 PyYAML）
# --------------------------------------------------------------------------- #
try:
    import yaml
except ImportError:  # pragma: no cover
    yaml = None


def _loader():
    assert yaml is not None

    class Base(yaml.SafeLoader):
        pass

    # YAML 1.1 会把 on/yes/off 解析为布尔，导致顶层键 "on" 丢失；只保留 true/false。
    Base.yaml_implicit_resolvers = {
        ch: [e for e in entries if e[0] != "tag:yaml.org,2002:bool"]
        for ch, entries in yaml.SafeLoader.yaml_implicit_resolvers.items()
    }
    Base.add_implicit_resolver(
        "tag:yaml.org,2002:bool",
        re.compile(r"^(?:true|false|True|False|TRUE|FALSE)$"),
        list("tTfF"),
    )

    class Strict(Base):
        def construct_mapping(self, node, deep=False):
            seen = set()
            for key_node, _ in node.value:
                key = self.construct_object(key_node, deep=deep)
                if key in seen:
                    raise ValueError(f"duplicate key: {key}")
                seen.add(key)
            return super().construct_mapping(node, deep)

    return Strict


@unittest.skipIf(yaml is None, "PyYAML 不可用：静态 workflow 结构契约显式 SKIP（不等于通过）")
class FormalWorkflowStructureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert yaml is not None
        cls.wf = yaml.load(raw(CI_YML), Loader=_loader())
        cls.text = raw(CI_YML)

    def test_toplevel_shape_and_jobs(self):
        self.assertEqual(self.wf["name"], "CI")
        self.assertEqual(
            set(self.wf), {"name", "on", "permissions", "concurrency", "env", "jobs"}
        )
        self.assertEqual(set(self.wf["jobs"]),
                         {"changes", "docs-check", "build-test",
                          "perf-regression", "result"})

    def test_triggers(self):
        on = self.wf["on"]
        self.assertEqual(set(on), {"pull_request", "push", "workflow_dispatch"})
        self.assertEqual(on["push"]["branches"], ["main"])
        self.assertEqual(on["pull_request"]["branches"], ["main"])
        self.assertIsNone(on["workflow_dispatch"], "手动重跑入口无 inputs")

    def test_permissions_minimal(self):
        self.assertEqual(self.wf["permissions"], {})
        for name, job in self.wf["jobs"].items():
            with self.subTest(job=name):
                self.assertNotIn("write", str(job.get("permissions", {})))
                self.assertEqual(job.get("permissions"), {"contents": "read"})

    def test_no_reusable_jobs_anywhere(self):
        for name, job in self.wf["jobs"].items():
            with self.subTest(job=name):
                self.assertNotIn("uses", job, f"{name} 不得是 reusable 调用")

    def test_local_jobs_runs_on_hosted(self):
        for name, job in self.wf["jobs"].items():
            with self.subTest(job=name):
                self.assertEqual(job["runs-on"], "ubuntu-24.04")

    def test_needs_graph_is_complete_and_acyclic(self):
        jobs = self.wf["jobs"]
        for name, job in jobs.items():
            for dep in job.get("needs", []) or []:
                with self.subTest(job=name, dep=dep):
                    self.assertIn(dep, jobs)
        self.assertIsNone(jobs["changes"].get("needs"))
        self.assertIsNone(jobs["docs-check"].get("needs"),
                          "docs-check 恒跑，不依赖 changes")
        self.assertEqual(jobs["build-test"]["needs"], ["changes"])
        # perf-regression 只依赖 changes 的 perf 位，不依赖 build-test。
        self.assertEqual(jobs["perf-regression"]["needs"], ["changes"])
        self.assertEqual(set(jobs["result"]["needs"]),
                         {"changes", "docs-check", "build-test", "perf-regression"},
                         "result 必须 needs 全部汇聚 job，不能漏键")

    def test_required_optional_disjoint_and_actual(self):
        result = self.wf["jobs"]["result"]
        run_text = "\n".join(step.get("run", "") for step in result["steps"])
        self.assertIn("scripts/ci/result.py", run_text)
        self.assertIn('"required": ["changes", "docs-check"]', run_text)
        self.assertIn('"optional": ["build-test", "perf-regression"]', run_text)
        # 声明的 required/optional 与常量一致且覆盖汇聚面（含 perf-regression，
        # perf 触发时由 result fail-closed 判定，预期跳过仅 expected_skips 列出）。
        self.assertTrue(REQUIRED, "required 不得为空（防全跳过变绿）")
        self.assertFalse(set(REQUIRED) & set(OPTIONAL))
        self.assertEqual(set(REQUIRED) | set(OPTIONAL), set(result["needs"]))
        self.assertEqual(set(OPTIONAL), set(HEAVY))

    def test_build_gate_and_result_always(self):
        build = self.wf["jobs"]["build-test"]
        self.assertEqual(build["name"], BUILD_CHECK_NAME)
        self.assertIn("needs.changes.result == 'success'", build["if"])
        self.assertIn("docs-only", build["if"])
        self.assertEqual(self.wf["jobs"]["result"]["if"], "${{ always() }}")

    def test_perf_gate_uses_changes_perf_output(self):
        perf = self.wf["jobs"]["perf-regression"]
        self.assertEqual(perf["name"], PERF_CHECK_NAME)
        self.assertIn("needs.changes.outputs.perf != 'false'", perf["if"])
        self.assertIn("always()", perf["if"],
                      "changes 失败时保守执行（等价 perf=true 规则）")
        for event in ("pull_request", "push"):
            with self.subTest(event=event):
                self.assertIn(event, perf["if"])

    def test_result_needs_covers_heavy_job(self):
        result = self.wf["jobs"]["result"]
        for job in HEAVY:
            with self.subTest(job=job):
                self.assertIn(job, result["needs"])
        results_expr = result["env"]["RESULTS_JSON"]
        for job in ("changes", "docs-check", *HEAVY):
            with self.subTest(job=job):
                self.assertIn(f'"{job}"', results_expr)

    def test_required_check_job_names_preserved(self):
        # job 名保持现役 required check 名，避免 required context 悬空。
        self.assertEqual(self.wf["jobs"]["changes"]["name"], "变更类型检测")
        self.assertEqual(self.wf["jobs"]["docs-check"]["name"], DOCS_CHECK_NAME)
        self.assertEqual(self.wf["jobs"]["build-test"]["name"], BUILD_CHECK_NAME)
        self.assertEqual(self.wf["jobs"]["perf-regression"]["name"], PERF_CHECK_NAME)
        self.assertNotIn("name", self.wf["jobs"]["result"],
                         "result 未设 display name → 唯一 required context = CI/result")


# --------------------------------------------------------------------------- #
# 正式 Recipe 不可少守卫（取代旧「影子 ↔ unit_test.yml」自耦合断言）
# --------------------------------------------------------------------------- #
class FormalRecipeGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = raw(CI_YML)
        cls.stripped = body(CI_YML)

    def test_retries_are_bounded_three_and_step_timeouts_present(self):
        self.assertEqual(self.stripped.count("for i in 1 2 3; do"), 2,
                         "ctest 与 smoke/reliability 各一轮有界重试循环")
        self.assertEqual(self.stripped.count("timeout-minutes: 10"), 1, "ctest step 10min")
        self.assertEqual(self.stripped.count("timeout-minutes: 12"), 1,
                         "smoke/reliability 合并 step 12min")
        self.assertEqual(self.stripped.count("timeout-minutes: 75"), 1, "build job 75min")

    def test_tool_self_tests_invoke_this_discovered_file(self):
        self.assertIn("python3 scripts/test_release_gate.py", self.stripped)
        self.assertIn("python3 -m unittest discover -s scripts/ci -p 'test_*.py'",
                      self.stripped)
        # 本文件必须真的能被上面的 discover 发现（入口自证）。
        self.assertRegex(os.path.basename(os.path.abspath(__file__)), r"^test_.*\.py$")
        self.assertEqual(os.path.abspath(__file__),
                         os.path.join(HERE, "test_formal_ci.py"))

    def test_boost_toolchain_prefix_is_reproduced_and_exported(self):
        for needle in (
            'bash scripts/ci/prepare_boost.sh "$BOOST_PREFIX"',
            "BOOST_ROOT=$BOOST_PREFIX",
            "CMAKE_PREFIX_PATH=$BOOST_PREFIX",
            "LD_LIBRARY_PATH=$BOOST_PREFIX/lib",
            "JOBS: 3",
        ):
            with self.subTest(needle=needle):
                self.assertIn(needle, self.stripped)

    def test_release_consumer_contract_preserved_and_perf_wiring_is_fail_closed(self):
        # release_gate.validate_main_ci 按 ci.yml 的 main push run 读取两个 context 成功。
        gate = raw(RELEASE_GATE)
        self.assertIn("ci.yml", gate)
        self.assertIn(BUILD_CHECK_NAME, gate)
        self.assertIn(PERF_CHECK_NAME, gate)
        # `编译 & 单元测试` 唯一 producer 不变；`性能回归检查` 为同 runner base/head 真实测量：
        # 只在完整 SHA 绑定、base/head 都测得干净、head 严格比较通过时成功，缺证据即 failure。
        self.assertEqual(self.stripped.count(f'name: "{BUILD_CHECK_NAME}"'), 1)
        self.assertEqual(self.stripped.count(f'name: "{PERF_CHECK_NAME}"'), 1)

    def test_perf_job_is_same_runner_fail_closed(self):
        # 性能 job 的信任边界、SHA 范围与 fail-closed 不变量（stdlib 文本守卫）。
        # 只看 perf-regression job 块，避免把 build job 的构建命令误判为本 job 行为。
        perf = _job_block(self.stripped, "perf-regression", "result")
        # 同 runner 真实测量：head driver + 两棵隔离构建树 + 只构建 unified_stress。
        self.assertIn("ci_perf_check.py", perf)
        self.assertIn("unified_stress", perf)
        self.assertIn("prepare_boost.sh", perf)
        self.assertIn("--strict-comparable", perf)
        self.assertIn("--build-dir", perf)
        self.assertIn("--commit-sha", perf)
        # 只构建目标、不跑全量 CTest。
        self.assertNotIn("ctest --timeout", perf)
        # subject/base：PR=head.sha/base.sha，push=github.sha/event.before；不冒充 merge commit。
        self.assertIn("github.event.pull_request.head.sha", perf)
        self.assertIn("github.event.pull_request.base.sha", perf)
        self.assertIn("github.sha", perf)
        self.assertIn("github.event.before", perf)
        # 完整 SHA 校验：零 SHA（首推）显式拒绝，不用 HEAD^ 猜测。
        self.assertIn("0000000000000000000000000000000000000000", perf)
        self.assertNotIn("HEAD^", perf)
        # base 无基线测量（NO_COMPARABLE_BASELINE 不是 PASS）；head 严格门禁。
        self.assertIn("--no-baseline-compare", perf)
        self.assertIn("--gate-enabled", perf)
        # 只读、无 secret/写权限/pull_request_target/self-hosted；checkout 不持久化凭据。
        self.assertNotIn("contents: write", self.stripped)
        self.assertNotIn("secrets:", self.stripped)
        self.assertNotIn("pull_request_target", self.stripped)
        self.assertNotIn("self-hosted", self.stripped)
        self.assertIn("persist-credentials: false", perf)
        # job timeout 有界（60min），六模块 45s × 2 threads。
        self.assertIn("timeout-minutes: 60", perf)
        self.assertIn("--dur 45", perf)
        self.assertIn("--threads 2", perf)
        # 不新增 artifact/cache/正式基线/变量。
        for forbidden in ("upload-artifact", "download-artifact", "actions/cache",
                          "record_baseline", "tests/baselines", "vars."):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, perf)

    def test_format_templates_render_to_strict_json(self):
        # 回归：result job 的 RESULTS_JSON 是 plain YAML scalar，format 模板内双引号前
        # 不得有 literal 反斜杠；以真实正常结果集合渲染后必须通过 strict
        # json.loads（仅断言子串无法捕获该缺陷，历史上 infra 候选曾用它把门禁吞成绿）。
        order = ("changes", "docs-check", *HEAVY)
        rs_tmpl = format_template(self.stripped, "RESULTS_JSON")
        self.assertNotIn('\\"', rs_tmpl, "RESULTS_JSON 模板不得含 literal 反斜杠转义")
        normal = {name: "success" for name in order}
        rendered = gha_format(rs_tmpl, *(normal[n] for n in order))
        self.assertEqual(json.loads(rendered), normal)


# --------------------------------------------------------------------------- #
# 结果判定端到端：直接执行本仓 result.py CLI（shared callee 已按 D3 删除，
# required/optional 语义由 result.py 承接，本类走真实 CLI 不复制契约）。
# --------------------------------------------------------------------------- #
def _result_cli(results, required, optional, expected_skips):
    payload = {
        "results": results,
        "required": required,
        "optional": optional,
        "expected_skips": expected_skips,
    }
    return run([sys.executable, RESULT], input=json.dumps(payload), env=ENV)


def _workflow_skips(classification, perf, event_name):
    """复刻 result job heredoc 的 expected_skips 推导（同一判定，不是第二份契约）。

    workflow 声明：build-test 仅 docs-only 可跳；perf-regression 仅 perf=false
    或 workflow_dispatch 可跳。本 helper 只在测试侧重放该推导，避免每个用例
    手抄字符串。
    """
    skips = []
    if classification == "docs-only":
        skips.append("build-test")
    if perf == "false" or event_name == "workflow_dispatch":
        skips.append("perf-regression")
    return skips


class ResultContractRoutingTests(unittest.TestCase):
    """needs.*.result → verdict 的端到端判定（与 result job heredoc 同一 payload 形状）。"""

    def _eval(self, results, skips=()):
        return _result_cli(results, REQUIRED, OPTIONAL, list(skips))

    def test_all_success_is_success(self):
        proc = self._eval({j: "success" for j in (*REQUIRED, *OPTIONAL)})
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(json.loads(proc.stdout)["verdict"], "success")

    def test_docs_only_expected_skips_are_success(self):
        # docs-only PR：build-test + perf-regression（perf=false 恒成立）均预期跳过。
        proc = self._eval(
            {"changes": "success", "docs-check": "success",
             "build-test": "skipped", "perf-regression": "skipped"},
            _workflow_skips("docs-only", "false", "pull_request"))
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(json.loads(proc.stdout)["verdict"], "success")

    def test_code_change_perf_false_skips_only_perf(self):
        # code + perf=false（如 tests-only 改动）：build-test 必跑，
        # perf-regression 预期跳过。
        proc = self._eval(
            {"changes": "success", "docs-check": "success",
             "build-test": "success", "perf-regression": "skipped"},
            _workflow_skips("code", "false", "push"))
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(json.loads(proc.stdout)["verdict"], "success")

    def test_perf_triggered_but_missing_is_failure(self):
        # perf=true 时 perf-regression 不在 expected_skips：skipped 判红（fail-closed）。
        proc = self._eval(
            {"changes": "success", "docs-check": "success",
             "build-test": "success", "perf-regression": "skipped"},
            _workflow_skips("code", "true", "push"))
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(json.loads(proc.stdout)["verdict"], "failure")

    def test_perf_triggered_failure_is_failure(self):
        proc = self._eval(
            {"changes": "success", "docs-check": "success",
             "build-test": "success", "perf-regression": "failure"},
            _workflow_skips("code", "true", "push"))
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(json.loads(proc.stdout)["verdict"], "failure")

    def test_dispatch_expected_skips_perf(self):
        # workflow_dispatch：perf job 的 if 限 pull_request/push → 必然 skipped，
        # workflow 声明为预期跳过；build-test 仍必跑。
        proc = self._eval(
            {"changes": "success", "docs-check": "success",
             "build-test": "success", "perf-regression": "skipped"},
            _workflow_skips("unknown", "true", "workflow_dispatch"))
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(json.loads(proc.stdout)["verdict"], "success")

    def test_code_build_test_failure_is_failure(self):
        proc = self._eval({"changes": "success", "docs-check": "success",
                           "build-test": "failure", "perf-regression": "success"},
                          [])
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(json.loads(proc.stdout)["verdict"], "failure")

    def test_undeclared_optional_skip_is_failure(self):
        # 未在 expected_skips 声明的 optional skipped 一律判红（不再按 classification 放行）。
        for results, skips in (
            ({"changes": "success", "docs-check": "success",
              "build-test": "skipped", "perf-regression": "success"}, []),
            ({"changes": "success", "docs-check": "success",
              "build-test": "success", "perf-regression": "skipped"}, []),
            ({"changes": "success", "docs-check": "success",
              "build-test": "skipped", "perf-regression": "skipped"},
             ["perf-regression"]),
        ):
            with self.subTest(results=results, skips=skips):
                proc = self._eval(results, skips)
                self.assertEqual(proc.returncode, 1)
                self.assertEqual(json.loads(proc.stdout)["verdict"], "failure")

    def test_required_skipped_is_failure(self):
        for results in (
            {"changes": "skipped", "docs-check": "success",
             "build-test": "success", "perf-regression": "success"},
            {"changes": "success", "docs-check": "skipped",
             "build-test": "success", "perf-regression": "success"},
        ):
            with self.subTest(results=results):
                proc = self._eval(results, ["build-test", "perf-regression"])
                self.assertEqual(proc.returncode, 1)

    def test_contract_rejects_exit_3(self):
        # 空 required / 集合外 job / 非法状态 / required∩optional /
        # expected_skips 超出 optional → rc 3 拒绝。
        for payload in (
            {"results": {"changes": "success"}, "required": []},
            {"results": {"changes": "success", "other": "success"},
             "required": ["changes"]},
            {"results": {"changes": "bogus"}, "required": ["changes"]},
            {"results": {"changes": "success"},
             "required": ["changes"], "optional": ["changes"]},
            {"results": {"changes": "success"},
             "required": ["changes"], "expected_skips": ["changes"]},
            {"results": {"changes": "success"},
             "required": ["changes"], "expected_skips": ["ghost"]},
            "not json",
            {"results": {}},
        ):
            with self.subTest(payload=payload):
                proc = run([sys.executable, RESULT], input=json.dumps(payload),
                           env=ENV)
                self.assertEqual(proc.returncode, 3, proc.stdout + proc.stderr)


# --------------------------------------------------------------------------- #
# docs-check job：文本契约（stdlib，恒跑；docs-check.yml 已并入 ci.yml）
# --------------------------------------------------------------------------- #
class DocsCheckTextContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = raw(CI_YML)
        cls.stripped = body(CI_YML)
        # 只看 docs-check job 块，避免把其他 job 的命令误判为本 job 行为。
        cls.job = _job_block(cls.stripped, "docs-check", "build-test")

    def test_identity_runner_permissions_and_timeout(self):
        self.assertIn('name: "文档检查（空格/链接/密钥）"', self.job)
        runs_on = re.findall(r"(?m)^\s*runs-on:\s*(\S+)\s*$", self.job)
        self.assertEqual(runs_on, ["ubuntu-24.04"])
        self.assertNotIn("self-hosted", self.job)
        self.assertIn("permissions:", self.job)
        self.assertIn("contents: read", self.job)
        self.assertNotRegex(self.job, r"(?m)^\s*[a-z-]+:\s*write\s*$")
        self.assertIn("timeout-minutes: 5", self.job)
        # docs-check 恒跑（docs-only PR 的唯一实质检查）：不得 needs 其他 job、不得带跳过 if。
        self.assertNotIn("needs:", self.job)
        self.assertNotIn("docs-only", self.job)

    def test_checkout_is_full_sha_pinned(self):
        self.assertIn(CHECKOUT_SHA, self.job)
        for uses in re.findall(r"(?m)^\s*uses:\s*(\S+)\s*$", self.job):
            with self.subTest(uses=uses):
                self.assertRegex(uses, SHA_PIN_RE)
        self.assertIn("persist-credentials: false", self.job)

    def test_run_blocks_do_not_inject_expressions_into_shell(self):
        for index, script in enumerate(_extract_run_blocks(raw(CI_YML))):
            with self.subTest(index=index):
                self.assertNotIn("${{", script,
                                 "diff SHA 必须走 env 输入，不得插进 shell 字符串")

    def test_no_paths_filtering_docs_check_is_unconditional(self):
        # docs-check.yml 的 paths 过滤已随并入删除：job 恒跑，不得偷回 paths/paths-ignore。
        self.assertNotIn("paths:", self.stripped)
        self.assertNotIn("paths-ignore", self.stripped)
        self.assertNotIn("硬闸门是仓库 Actions", self.text)

    def test_no_network_link_checker_added(self):
        for forbidden in ("lychee", "markdown-link-check", "markdownlint", "curl "):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.stripped)

    def test_scan_and_whitespace_guards_present(self):
        self.assertIn("git diff --check", self.job)
        self.assertIn("git grep -qEI", self.job)
        self.assertIn("fail-closed", self.job)
        # 迁入的 check_docs.py（链接/文件名/README 上限）必须真的接线。
        self.assertIn("scripts/ci/check_docs.py", self.job)
        self.assertIn("--readme-max 200", self.job)


# --------------------------------------------------------------------------- #
# docs-check：行为负例（真实 bash，有界）
# --------------------------------------------------------------------------- #
class DocsCheckBehaviorTests(unittest.TestCase):
    WHITESPACE = "git diff --check"
    SCAN = "secret-like material found"

    def _run_block(self, needle, workdir, extra_env=None):
        script = _block_containing(CI_YML, needle)
        fd, tmp = tempfile.mkstemp(suffix=".sh")
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            handle.write(script)
        try:
            env = dict(ENV, **(extra_env or {}))
            return run(["bash", tmp], env=env, cwd=workdir)
        finally:
            os.unlink(tmp)

    def test_whitespace_rejects_invalid_or_missing_sha(self):
        with tempfile.TemporaryDirectory() as td:
            proc = self._run_block(
                self.WHITESPACE, td,
                {"EVENT_NAME": "pull_request", "BASE_SHA": "not-a-sha"},
            )
            self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
            proc = self._run_block(self.WHITESPACE, td, {"EVENT_NAME": "push"})
            self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
            proc = self._run_block(
                self.WHITESPACE, td,
                {"EVENT_NAME": "push", "BEFORE": "0" * 40, "SHA": "short"},
            )
            self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)

    def test_whitespace_detects_and_passes_real_ranges(self):
        with tempfile.TemporaryDirectory() as repo:
            _init_repo(repo)
            base = _commit(repo, "docs/a.md", "a\n", "base")
            bad = _commit(repo, "docs/b.md", "trailing   \n", "bad")
            proc = self._run_block(
                self.WHITESPACE, repo,
                {"EVENT_NAME": "push", "BEFORE": base, "SHA": bad},
            )
            self.assertNotEqual(proc.returncode, 0, "真实空白错误必须被抓住")
            clean = _commit(repo, "docs/c.md", "clean\n", "clean")
            proc = self._run_block(
                self.WHITESPACE, repo,
                {"EVENT_NAME": "push", "BEFORE": bad, "SHA": clean},
            )
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_whitespace_missing_remote_base_fails_closed(self):
        # 范围 rev 不可达时不得静默放行。
        with tempfile.TemporaryDirectory() as repo:
            _init_repo(repo)
            head = _commit(repo, "docs/a.md", "a\n", "base")
            proc = self._run_block(
                self.WHITESPACE, repo,
                {"EVENT_NAME": "pull_request", "BASE_SHA": "b" * 40},
            )
            self.assertNotEqual(proc.returncode, 0)
            self.assertNotEqual(proc.returncode, 2, "格式合法但 rev 不可达 → 由 git 失败")
            self.assertNotEqual(head, "")

    def test_secret_scan_fails_closed_when_grep_errors(self):
        with tempfile.TemporaryDirectory() as td:  # 非 git 仓库 → git grep rc=128
            proc = self._run_block(self.SCAN, td)
            self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
            self.assertIn("fail-closed", proc.stderr)

    def test_secret_scan_detects_synthetic_token_and_passes_clean(self):
        # 片段拼接构造 synthetic token：本测试源码本身不落真实凭据（仓内扫描不会命中）。
        token = "ghp_" + "A" * 24
        with tempfile.TemporaryDirectory() as repo:
            _init_repo(repo)
            _commit(repo, "docs/clean.md", "no secrets here\n", "clean")
            proc = self._run_block(self.SCAN, repo)
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            _commit(repo, "docs/leak.md", f"token={token}\n", "leak")
            proc = self._run_block(self.SCAN, repo)
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            self.assertNotIn(token, proc.stdout + proc.stderr, "扫描失败不得把命中值写入公开日志")


if __name__ == "__main__":
    unittest.main(verbosity=2)
