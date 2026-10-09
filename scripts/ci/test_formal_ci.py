#!/usr/bin/env python3
"""bbtools-coroutine 正式 hosted CI / docs-check 的离线冒烟与契约测试（stdlib 为主，PyYAML 可选）。

本文件原名 `test_shadow_ci.py`（影子身份）。hosted shadow Layer1 已升为**唯一**正式普通门禁
`.github/workflows/unit_test.yml`、影子入口已删除，本文件随之改名为 `test_formal_ci.py`：
移除影子身份断言与「影子 ↔ 旧 unit_test.yml」自耦合断言，改为对**实际正式 Recipe** 的不可少
守卫（构建判据 / retry 上界 / 工具自测调用入口 / Boost 前缀 / release_gate 消费者契约 /
format 模板严格 JSON / docs-check 属性与 fail-closed 行为）。

会被正式 `unit_test.yml` 的 `python3 -m unittest discover -s scripts/ci -p 'test_*.py'`
**自动发现**，无需新增 run_tests.py 入口（避免碰既有测试入口）。本地只验证「CI 配方 / 分类路由 /
workflow 静态契约 / docs-check 行为」，**不跑 C++ 全量构建**（真实构建走父级授权的 PR CI）。

覆盖：
- 配方守卫：脚本 bash -n、参数校验、fail-closed 拒绝已存在前缀 / 未知 revision / 非法 jobs；
- changed_files 路由：push/PR 真实 diff 归一、未知保守回退、超预算整集回退；
- 正式 workflow 文本契约（stdlib，恒跑，直接读真实文件）：触发面 / 权限 / SHA pin / hosted
  runs-on / concurrency / 复用 callee / required-optional / docs-only 门控 / always 汇聚 /
  无 artifact / 无 cache / 无 perf / 无 sanitizer 长测 / 影子已退役；
- 正式 workflow 结构契约（需 PyYAML，缺省显式 SKIP，不等于通过）；
- 正式 Recipe 不可少守卫（含 release_gate 消费者契约）；
- format 模板严格 JSON 回归：`event_json`/`results_json` 从**实际模板**渲染后必须通过 strict
  `json.loads`，并经真实 callee 输入契约接受（防 literal 反斜杠缺陷复发）；
- 分类路由冒烟（需 `BBT_CI_SHARED_DIR` 指向已发布 framework 的 scripts/ci/shared）：用**同一个
  真实 cli.py** 校验正式传入的受限输入与 required/optional 判定，覆盖 docs-only / code / unknown /
  planfail / buildfail / skipped / cancelled；
- docs-check：文本契约（hosted / SHA pin / trigger paths / 无表达式注入 / 无网络链接检查工具）与
  行为负例（SHA 格式非法 fail-closed、真实空白错误可抓、空白范围干净放行、密钥扫描 rc>1
  fail-closed、synthetic token 命中）。

运行：
  PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s scripts/ci -p 'test_*.py'
  # 需要 PyYAML 才跑结构契约；需要真实 callee 逻辑才跑分类路由冒烟，否则显式 SKIP。
  PYTHONDONTWRITEBYTECODE=1 BBT_CI_SHARED_DIR=<framework>/scripts/ci/shared \\
      /usr/bin/python3 -m unittest discover -s scripts/ci -p 'test_*.py'
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
UNIT_YML = os.path.join(WORKFLOWS, "unit_test.yml")
DOCS_YML = os.path.join(WORKFLOWS, "docs-check.yml")
# 影子资产（本候选已授权退役）：入口与说明都必须不存在。
SHADOW_YML = os.path.join(WORKFLOWS, "ci-shadow-v1.yml")
SHADOW_DOC = os.path.join(WORKTREE, "agent-docs", "ci-shadow-v1.md")
FORMAL_DOC = os.path.join(WORKTREE, "agent-docs", "formal-ci.md")
SHADOW_TEST = os.path.join(HERE, "test_shadow_ci.py")
RELEASE_GATE = os.path.join(WORKTREE, "scripts", "release_gate.py")
PREPARE_BOOST = os.path.join(HERE, "prepare_boost.sh")
CHANGED = os.path.join(HERE, "changed_files.py")

CALLEE_SHA = "1c0b0fb0ebc8e7ca3888aa4f6a74d1baecf08cc7"
CALLEE = f"yqm-307/bbt-framework/.github/workflows/bbtools-classify-v1.yml@{CALLEE_SHA}"
CHECKOUT_SHA = "11d5960a326750d5838078e36cf38b85af677262"
SHA_PIN_RE = re.compile(r"^[^@\s]+@[0-9a-f]{40}$")

REQUIRED = ["changes", "plan"]
OPTIONAL = ["build"]
HEAVY = ("build",)
BUILD_CHECK_NAME = "编译 & 单元测试"
PERF_CHECK_NAME = "性能回归检查"

DOCS_PATHS = (
    "'**.md'", "'docs/**'", "'agent-docs/**'", "'LICENSE'",
    "'**/*.png'", "'**/*.jpg'", "'**/*.svg'",
)

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


def format_template(key, path=UNIT_YML):
    """从实际 workflow 取 `key:` 行里 format('...') 的单引号模板（模板内不含单引号）。"""
    for line in raw(path).splitlines():
        if re.match(rf"\s*{re.escape(key)}:", line) and "format(" in line:
            match = re.search(r"format\('([^']*)'", line)
            if match:
                return match.group(1)
    raise AssertionError(f"未在 {os.path.basename(path)} 找到 {key} 的 format 模板")


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
        for path in (CHANGED, os.path.abspath(__file__)):
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
            head = _commit(repo, "agent-docs/formal-ci.md", "n", "note")
            proc, values = _changed_files(
                repo,
                {"EVENT_NAME": "pull_request", "BASE_REF": "main", "SHA": head},
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(
                json.loads(values["changed_files_json"]), ["agent-docs/formal-ci.md"]
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
    """不依赖 PyYAML 的静态契约：直接读真实 .github/workflows/unit_test.yml 文本。"""

    @classmethod
    def setUpClass(cls):
        cls.text = raw(UNIT_YML)
        cls.stripped = body(UNIT_YML)

    def test_name_and_identity(self):
        self.assertIn("\nname: CI\n", self.text, "workflow 名必须保持 CI")
        self.assertIn(".github/workflows/unit_test.yml", self.text,
                      "path 必须保持 unit_test.yml——release_gate 按此 path 读取")

    def test_trigger_is_main_pr_and_push_only(self):
        self.assertIn("branches: [main]", self.stripped)
        for forbidden in ("workflow_dispatch", "schedule", "merge_group",
                          "ci/issue-50-hosted-shadow"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.stripped)

    def test_retired_shadow_assets_are_gone_no_double_run(self):
        for path in (SHADOW_YML, SHADOW_DOC, SHADOW_TEST):
            with self.subTest(path=os.path.basename(path)):
                self.assertFalse(os.path.exists(path), "影子资产已授权退役，不得残留")
        self.assertTrue(os.path.isfile(FORMAL_DOC), "正式说明必须存在")
        self.assertTrue(os.path.isfile(os.path.abspath(__file__)))
        self.assertIn("agent-docs/formal-ci.md", self.text)
        # 未被授权改动的邻居 workflow 必须仍在。
        for keep in ("memery_test_info.yml", "release.yml"):
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

    def test_reuses_published_callee_not_local(self):
        self.assertIn(CALLEE, self.stripped)
        self.assertNotIn("./.github/workflows", self.stripped)
        self.assertEqual(
            self.stripped.count(CALLEE), 2, "应在 plan 与 result 各复用一次 callee"
        )
        self.assertIn(CHECKOUT_SHA, self.stripped)

    def test_build_job_keeps_required_check_name_as_sole_producer(self):
        # 旧 required context `编译 & 单元测试` 必须恰有一个真实 producer，且不靠空成功 alias 伪造。
        self.assertEqual(self.stripped.count(f'name: "{BUILD_CHECK_NAME}"'), 1)
        self.assertNotIn("continue-on-error", self.stripped)
        self.assertNotIn(f'name: "{PERF_CHECK_NAME}"', self.stripped,
                         "性能路径本轮冻结：不得伪造 `性能回归检查` 结果")
        self.assertIn('name: "编译 & 单元测试"', self.text)

    def test_required_and_optional_are_disjoint_and_build_is_optional(self):
        self.assertIn('required_checks_json: \'["changes","plan"]\'', self.stripped)
        self.assertIn('optional_checks_json: \'["build"]\'', self.stripped)
        # 重型 build 为 optional：仅 docs-only 允许显式跳过，且必须等 plan 成功。
        self.assertIn("needs.plan.outputs.classification != 'docs-only'", self.stripped)
        self.assertIn("needs.plan.result == 'success'", self.stripped)
        self.assertNotIn("docs-only 变更直接通过", self.stripped,
                         "docs-only 走显式跳过，不用空成功 echo 伪造 job 结果")

    def test_result_job_aggregates_always_and_passes_all_job_results(self):
        self.assertIn("if: ${{ always() }}", self.stripped)
        self.assertIn(
            "format('{{\"changes\":\"{0}\",\"plan\":\"{1}\",\"build\":\"{2}\"}}'",
            self.stripped,
        )
        for ref in ("needs.changes.result", "needs.plan.result", "needs.build.result"):
            with self.subTest(ref=ref):
                self.assertIn(ref, self.stripped)

    def test_result_job_has_conservative_fallbacks(self):
        self.assertIn("|| '[]'", self.stripped)
        self.assertIn("|| 'unknown'", self.stripped)
        self.assertIn('format(\'{{"name":"{0}"}}\', github.event_name)', self.stripped)

    def test_no_perf_release_memcheck_artifact_or_cache(self):
        # 性能 / 基线 / 发布 / memcheck / artifact / cache 都不在本正式候选内。
        for forbidden in (
            "ci_perf_check.py", "perf-baseline", "tests/baselines",
            "run_memcheck", "record_baseline", "release_gate.py publish",
            "unified_stress", "upload-artifact", "download-artifact",
            "actions/cache", "ccache",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.stripped)

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
            "90s bin/unit_test/Test_smoke --log_level=test_suite",
            "120s bin/unit_test/Test_reliability --log_level=test_suite",
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
        cls.wf = yaml.load(raw(UNIT_YML), Loader=_loader())
        cls.text = raw(UNIT_YML)

    def test_toplevel_shape_and_jobs(self):
        self.assertEqual(self.wf["name"], "CI")
        self.assertEqual(
            set(self.wf), {"name", "on", "permissions", "concurrency", "env", "jobs"}
        )
        self.assertEqual(set(self.wf["jobs"]), {"changes", "plan", "build", "result"})

    def test_triggers(self):
        on = self.wf["on"]
        self.assertEqual(set(on), {"pull_request", "push"})
        self.assertEqual(on["push"]["branches"], ["main"])
        self.assertEqual(on["pull_request"]["branches"], ["main"])

    def test_permissions_minimal(self):
        self.assertEqual(self.wf["permissions"], {})
        for name, job in self.wf["jobs"].items():
            with self.subTest(job=name):
                self.assertNotIn("write", str(job.get("permissions", {})))
                if "uses" not in job:
                    self.assertEqual(job.get("permissions"), {"contents": "read"})

    def test_reusable_jobs_have_no_runs_on_and_match_callee(self):
        for name, job in self.wf["jobs"].items():
            if "uses" not in job:
                continue
            with self.subTest(job=name):
                self.assertNotIn("runs-on", job, f"{name} 为 reusable 调用")
                self.assertEqual(job["uses"], CALLEE)
                self.assertNotIn("secrets", job)
                self.assertEqual(job["with"]["profile"], "hosted")
                self.assertIn("github.sha", job["with"]["source_sha"])

    def test_local_jobs_runs_on_hosted(self):
        for name, job in self.wf["jobs"].items():
            if "uses" in job:
                continue
            with self.subTest(job=name):
                self.assertEqual(job["runs-on"], "ubuntu-24.04")

    def test_needs_graph_is_complete_and_acyclic(self):
        jobs = self.wf["jobs"]
        for name, job in jobs.items():
            for dep in job.get("needs", []) or []:
                with self.subTest(job=name, dep=dep):
                    self.assertIn(dep, jobs)
        self.assertEqual(jobs["plan"]["needs"], ["changes"])
        self.assertEqual(jobs["build"]["needs"], ["changes", "plan"])
        self.assertEqual(set(jobs["result"]["needs"]), set(jobs) - {"result"},
                         "result 必须 needs 全部真实 job，不能漏键")

    def test_required_optional_disjoint_and_actual(self):
        for name in ("plan", "result"):
            with self.subTest(job=name):
                with_block = self.wf["jobs"][name]["with"]
                required = json.loads(with_block["required_checks_json"])
                optional = json.loads(with_block["optional_checks_json"])
                self.assertEqual(required, REQUIRED)
                self.assertEqual(optional, OPTIONAL)
                self.assertTrue(required, "required 不得为空（防全跳过变绿）")
                self.assertFalse(set(required) & set(optional))
                # 声明的 job id 必须真实存在，且 required∪optional 覆盖全部重型 job。
                self.assertTrue(set(required) | set(optional) <= set(self.wf["jobs"]))
                self.assertEqual(set(optional), set(HEAVY))

    def test_build_gate_and_result_always(self):
        build = self.wf["jobs"]["build"]
        self.assertEqual(build["name"], BUILD_CHECK_NAME)
        self.assertIn("needs.plan.result == 'success'", build["if"])
        self.assertIn("docs-only", build["if"])
        self.assertEqual(self.wf["jobs"]["result"]["if"], "${{ always() }}")

    def test_result_needs_covers_heavy_job(self):
        result = self.wf["jobs"]["result"]
        for job in HEAVY:
            with self.subTest(job=job):
                self.assertIn(job, result["needs"])
        results_expr = result["with"]["results_json"]
        for job in ("changes", *HEAVY):
            with self.subTest(job=job):
                self.assertIn(f'"{job}"', results_expr)


# --------------------------------------------------------------------------- #
# 正式 Recipe 不可少守卫（取代旧「影子 ↔ unit_test.yml」自耦合断言）
# --------------------------------------------------------------------------- #
class FormalRecipeGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = raw(UNIT_YML)
        cls.stripped = body(UNIT_YML)

    def test_retries_are_bounded_three_and_step_timeouts_present(self):
        self.assertEqual(self.stripped.count("for i in 1 2 3; do"), 3,
                         "ctest/smoke/reliability 各有且仅有 3 次有界重试")
        self.assertEqual(self.stripped.count("timeout-minutes: 10"), 1, "ctest step 10min")
        self.assertEqual(self.stripped.count("timeout-minutes: 8"), 2,
                         "smoke 与 reliability step 各 8min")
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

    def test_release_consumer_contract_is_preserved_and_perf_is_frozen(self):
        # release_gate.validate_main_ci 按 unit_test.yml 的 main push run 读取两个 context 成功。
        gate = raw(RELEASE_GATE)
        self.assertIn("unit_test.yml", gate)
        self.assertIn(BUILD_CHECK_NAME, gate)
        self.assertIn(PERF_CHECK_NAME, gate)
        # 本候选保留 `编译 & 单元测试` 唯一 producer，但**不**承载 `性能回归检查`
        # → required pending，Release fail-closed（本轮授权的性能冻结状态）。
        self.assertEqual(self.stripped.count(f'name: "{BUILD_CHECK_NAME}"'), 1)
        self.assertNotIn(PERF_CHECK_NAME, self.stripped)

    def test_format_templates_render_to_strict_json(self):
        # 回归：result job 的 event_json/results_json 是 plain YAML scalar，format 模板内双引号前
        # 不得有 literal 反斜杠；以真实正常结果集合与 fallback event 渲染后必须通过 strict
        # json.loads（仅断言子串无法捕获该缺陷，历史上 infra 候选曾用它把门禁吞成绿）。
        ev_tmpl = format_template("event_json")
        self.assertNotIn('\\"', ev_tmpl, "event_json 模板不得含 literal 反斜杠转义")
        self.assertEqual(json.loads(gha_format(ev_tmpl, "push")), {"name": "push"})

        order = ("changes", "plan", *HEAVY)
        rs_tmpl = format_template("results_json")
        self.assertNotIn('\\"', rs_tmpl, "results_json 模板不得含 literal 反斜杠转义")
        normal = {name: "success" for name in order}
        rendered = gha_format(rs_tmpl, *(normal[n] for n in order))
        self.assertEqual(json.loads(rendered), normal)


# --------------------------------------------------------------------------- #
# 分类路由冒烟：真实复用 framework callee 逻辑（不复制契约）
# --------------------------------------------------------------------------- #
SHARED = os.environ.get("BBT_CI_SHARED_DIR", "")


def _formal_payload(changed_files_json, classifier_status="ok"):
    return {
        "repo": "yqm-307/bbtools-coroutine",
        "source_sha": "a" * 40,
        "profile": "hosted",
        "concurrency": "2",
        "timeout_minutes": "60",
        "event_json": '{"name":"pull_request","base_ref":"main"}',
        "changed_files_json": changed_files_json,
        "required_checks_json": json.dumps(REQUIRED),
        "optional_checks_json": json.dumps(OPTIONAL),
        "classifier_status": classifier_status,
    }


@unittest.skipUnless(
    SHARED and os.path.isfile(os.path.join(SHARED, "cli.py")),
    "BBT_CI_SHARED_DIR 未指向已发布 callee scripts/ci/shared：分类路由冒烟显式 SKIP",
)
class ClassificationRoutingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cli = os.path.join(SHARED, "cli.py")

    def _classify(self, changed, status="ok"):
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as handle:
            json.dump(_formal_payload(changed, status), handle)
            path = handle.name
        proc = run([sys.executable, self.cli, "classify", "--file", path], env=ENV)
        os.unlink(path)
        return proc

    def _evaluate(self, plan, results):
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as handle:
            json.dump({"plan": plan, "results": results}, handle)
            path = handle.name
        proc = run([sys.executable, self.cli, "evaluate", "--file", path], env=ENV)
        os.unlink(path)
        return proc

    def _plan_for(self, changed, status="ok"):
        proc = self._classify(changed, status)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        return json.loads(proc.stdout)["plan"]

    def _verdict(self, changed, results, status="ok"):
        proc = self._evaluate(self._plan_for(changed, status), results)
        return proc, json.loads(proc.stdout)["verdict"]

    # ---- 分类 ----
    def test_docs_only_routing(self):
        proc = self._classify('["agent-docs/formal-ci.md"]')
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(json.loads(proc.stdout)["classification"], "docs-only")

    def test_code_routing(self):
        self.assertEqual(
            json.loads(self._classify('["bbt/coroutine/Scheduler.cc"]').stdout)[
                "classification"
            ],
            "code",
        )

    def test_classifier_failure_is_unknown_not_docs(self):
        proc = self._classify('["agent-docs/formal-ci.md"]', status="failed")
        self.assertEqual(json.loads(proc.stdout)["classification"], "unknown")
        self.assertEqual(
            json.loads(self._classify("[]").stdout)["classification"], "unknown"
        )

    # ---- 结果汇聚路径 ----
    def test_docs_only_allows_build_skip_and_is_success(self):
        proc, verdict = self._verdict(
            '["agent-docs/formal-ci.md"]',
            {"changes": "success", "plan": "success", "build": "skipped"},
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(verdict, "success")

    def test_code_build_success_is_success(self):
        proc, verdict = self._verdict(
            '["bbt/x.cc"]',
            {"changes": "success", "plan": "success", "build": "success"},
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(verdict, "success")

    def test_code_build_failure_is_failure(self):
        proc, verdict = self._verdict(
            '["bbt/x.cc"]',
            {"changes": "success", "plan": "success", "build": "failure"},
        )
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(verdict, "failure")

    def test_code_build_skipped_is_failure(self):
        proc, verdict = self._verdict(
            '["bbt/x.cc"]',
            {"changes": "success", "plan": "success", "build": "skipped"},
        )
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(verdict, "failure")

    def test_code_build_cancelled_is_failure(self):
        proc, verdict = self._verdict(
            '["bbt/x.cc"]',
            {"changes": "success", "plan": "success", "build": "cancelled"},
        )
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(verdict, "failure")

    def test_plan_failure_is_failure(self):
        # plan 失败 → build 被 if 挡成 skipped；required plan 非 success 必须判 failure。
        proc, verdict = self._verdict(
            '["bbt/x.cc"]',
            {"changes": "success", "plan": "failure", "build": "skipped"},
        )
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(verdict, "failure")

    def test_required_skipped_is_failure(self):
        proc, verdict = self._verdict(
            '["bbt/x.cc"]',
            {"changes": "success", "plan": "skipped", "build": "skipped"},
        )
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(verdict, "failure")

    def test_required_cancelled_is_failure(self):
        proc, verdict = self._verdict(
            '["bbt/x.cc"]',
            {"changes": "cancelled", "plan": "success", "build": "success"},
        )
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(verdict, "failure")

    def test_unknown_classification_forbids_build_skip(self):
        proc, verdict = self._verdict(
            "[]",
            {"changes": "success", "plan": "success", "build": "skipped"},
            status="failed",
        )
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(verdict, "failure")

    def test_unknown_job_key_is_rejected(self):
        proc = self._evaluate(
            self._plan_for('["bbt/x.cc"]'),
            {"changes": "success", "plan": "success", "build": "success",
             "perf-regression": "success"},
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertEqual(json.loads(proc.stdout)["code"], "E_RESULT_UNKNOWN_JOB")

    def test_rendered_workflow_inputs_pass_real_callee(self):
        # 用**实际 workflow 模板**渲染出的 event_json / results_json（真实正常结果集合 +
        # fallback event）按 workflow_call 输入喂给真实 cli.py：契约必须接受并给 verdict=success。
        order = ("changes", "plan", *HEAVY)
        results = {name: "success" for name in order}
        rendered_rs = gha_format(format_template("results_json"), *(results[n] for n in order))
        rendered_ev = gha_format(format_template("event_json"), "push")
        self.assertEqual(json.loads(rendered_rs), results)
        self.assertEqual(json.loads(rendered_ev), {"name": "push"})

        payload = _formal_payload('["bbt/x.cc"]')
        payload["event_json"] = rendered_ev
        payload["results_json"] = rendered_rs
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as handle:
            json.dump(payload, handle)
            path = handle.name
        proc = run([sys.executable, self.cli, "classify", "--file", path], env=ENV)
        os.unlink(path)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        out = json.loads(proc.stdout)
        self.assertEqual(out["classification"], "code")
        self.assertEqual(out["evaluation"]["verdict"], "success")


# --------------------------------------------------------------------------- #
# docs-check：文本契约（stdlib，恒跑）
# --------------------------------------------------------------------------- #
class DocsCheckTextContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = raw(DOCS_YML)
        cls.stripped = body(DOCS_YML)

    def test_identity_runner_permissions_and_timeout(self):
        self.assertIn("\nname: Docs Check\n", self.text)
        self.assertIn('name: "文档检查（空格/链接/密钥）"', self.text)
        runs_on = re.findall(r"(?m)^\s*runs-on:\s*(\S+)\s*$", self.stripped)
        self.assertEqual(set(runs_on), {"ubuntu-24.04"})
        self.assertNotIn("self-hosted", self.stripped)
        self.assertIn("permissions:", self.stripped)
        self.assertIn("contents: read", self.stripped)
        self.assertNotRegex(self.stripped, r"(?m)^\s*[a-z-]+:\s*write\s*$")
        self.assertIn("timeout-minutes: 5", self.stripped)

    def test_trigger_paths_preserved(self):
        self.assertIn("branches: [main]", self.stripped)
        for path in DOCS_PATHS:
            with self.subTest(path=path):
                self.assertEqual(self.stripped.count(path), 2,
                                 "pull_request 与 push 各保留同一路径清单")

    def test_checkout_is_full_sha_pinned(self):
        self.assertIn(CHECKOUT_SHA, self.stripped)
        for uses in re.findall(r"(?m)^\s*uses:\s*(\S+)\s*$", self.stripped):
            with self.subTest(uses=uses):
                self.assertRegex(uses, SHA_PIN_RE)
        self.assertIn("persist-credentials: false", self.text)

    def test_run_blocks_do_not_inject_expressions_into_shell(self):
        blocks = _extract_run_blocks(self.text)
        self.assertTrue(blocks)
        for index, script in enumerate(blocks):
            with self.subTest(index=index):
                self.assertNotIn("${{", script,
                                 "diff SHA 必须走 env 输入，不得插进 shell 字符串")

    def test_no_false_paths_ignore_or_hard_gate_claims(self):
        self.assertNotIn("paths-ignore", self.stripped,
                         "docs-check 自身不得使用 paths-ignore")
        self.assertNotIn("与 unit_test.yml 的 paths-ignore 互补", self.text,
                         "旧注释的 paths-ignore 互补说法不实")
        self.assertNotIn("硬闸门是仓库 Actions", self.text)
        # approval_policy 如实回读，且不当作隔离保证。
        self.assertIn("all_external_contributors", self.text)
        self.assertIn("2026-10-09", self.text)
        self.assertIn("不构成", self.text)

    def test_no_network_link_checker_added(self):
        for forbidden in ("lychee", "markdown-link-check", "markdownlint", "curl "):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.stripped)

    def test_scan_and_whitespace_guards_present(self):
        self.assertIn("git diff --check", self.stripped)
        self.assertIn("git grep -qEI", self.stripped)
        self.assertIn("fail-closed", self.stripped)

    def test_inline_scripts_pass_bash_n(self):
        for script in _extract_run_blocks(self.text):
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
# docs-check：行为负例（真实 bash，有界）
# --------------------------------------------------------------------------- #
class DocsCheckBehaviorTests(unittest.TestCase):
    WHITESPACE = "git diff --check"
    SCAN = "secret-like material found"

    def _run_block(self, needle, workdir, extra_env=None, with_git=False):
        script = _block_containing(DOCS_YML, needle)
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
