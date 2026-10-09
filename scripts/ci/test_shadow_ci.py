#!/usr/bin/env python3
"""ci-shadow-v1 离线冒烟与直接耦合测试（stdlib 为主，PyYAML 可选增强）。

本文件按 `scripts/ci/` 现有约定命名（`test_*.py`），因此会被现役 unit_test.yml 的
`python3 -m unittest discover -s scripts/ci -p 'test_*.py'` **自动发现**，无需新增
run_tests.py 入口（避免碰既有测试入口）。本地只验证「CI 配方 / 分类路由 / 静态契约」，
**不跑 C++ 全量构建**（真实构建走父级授权的 PR CI）。

覆盖：
- 配方守卫：脚本 bash -n、参数校验、fail-closed 拒绝已存在前缀/未知 revision；
- changed_files 路由：push/PR 真实 diff 归一、未知保守回退、超预算整集回退；
- 影子 workflow 文本契约（stdlib，恒跑）：触发面/权限/SHA pin/hosted runs-on/
  concurrency 独立/复用 callee/required/optional/docs-only/always 汇聚/无 artifact/无 cache；
- 影子 workflow 结构契约（需 PyYAML，缺省显式 SKIP，不等于通过）；
- 与现役 .github/workflows/unit_test.yml 的直接耦合：构建选项/ctest/smoke/reliability/
  工具自测命令逐项对齐（防漂移）；
- 分类路由冒烟（需 BBT_CI_SHARED_DIR 指向已发布 framework 的 scripts/ci/shared）：
  用**同一个真实 cli.py** 校验影子传入的受限输入与 required/optional 判定，本仓不复制契约。

运行：
  PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s scripts/ci -p 'test_*.py'
  # 需要 PyYAML 才跑结构契约；需要真实 callee 逻辑才跑分类路由冒烟，否则显式 SKIP。
  PYTHONDONTWRITEBYTECODE=1 BBT_CI_SHARED_DIR=<framework>/scripts/ci/shared \\
      python3 -m unittest discover -s scripts/ci -p 'test_*.py'
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
SHADOW = os.path.join(WORKTREE, ".github", "workflows", "ci-shadow-v1.yml")
UNIT_YML = os.path.join(WORKTREE, ".github", "workflows", "unit_test.yml")
PREPARE_BOOST = os.path.join(HERE, "prepare_boost.sh")
CHANGED = os.path.join(HERE, "changed_files.py")

CALLEE_SHA = "1c0b0fb0ebc8e7ca3888aa4f6a74d1baecf08cc7"
CALLEE = f"yqm-307/bbt-framework/.github/workflows/bbtools-classify-v1.yml@{CALLEE_SHA}"
CHECKOUT_SHA = "11d5960a326750d5838078e36cf38b85af677262"
SHA_PIN_RE = re.compile(r"^[^@\s]+@[0-9a-f]{40}$")

ENV = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")


def raw(path: str) -> str:
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def body(path: str) -> str:
    """去掉说明性注释行后的实际 YAML 内容（注释中可提及被禁写法）。"""
    return "\n".join(
        line for line in raw(path).splitlines() if not line.lstrip().startswith("#")
    )


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
    _git(repo, "config", "user.email", "shadow@test")
    _git(repo, "config", "user.name", "shadow")
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
    out_path = os.path.join(repo, ".shadow-output")
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
                 "REF_NAME": "ci/issue-50-hosted-shadow"},
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(values["classifier_status"], "ok")
            self.assertEqual(json.loads(values["changed_files_json"]), ["bbt/coroutine/x.cc"])
            self.assertEqual(
                json.loads(values["event_json"]),
                {"name": "push", "head_ref": "ci/issue-50-hosted-shadow"},
            )

    def test_pull_request_three_dot(self):
        with tempfile.TemporaryDirectory() as repo:
            _init_repo(repo)
            base = _commit(repo, "docs/a.md", "a", "base")
            _git(repo, "update-ref", "refs/remotes/origin/main", base)
            _git(repo, "checkout", "-q", "-b", "feat")
            head = _commit(repo, "agent-docs/ci-shadow-v1.md", "n", "note")
            proc, values = _changed_files(
                repo,
                {"EVENT_NAME": "pull_request", "BASE_REF": "main", "SHA": head},
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(
                json.loads(values["changed_files_json"]), ["agent-docs/ci-shadow-v1.md"]
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
# 影子 workflow 文本契约（stdlib，恒跑，直接读真实文件）
# --------------------------------------------------------------------------- #
class WorkflowTextContractTests(unittest.TestCase):
    """不依赖 PyYAML 的静态契约：直接读真实 .github/workflows/ci-shadow-v1.yml 文本。"""

    @classmethod
    def setUpClass(cls):
        cls.text = raw(SHADOW)
        cls.stripped = body(SHADOW)

    def test_name_and_identity(self):
        self.assertIn("\nname: ci-shadow-v1\n", self.text)

    def test_trigger_is_branch_push_and_main_pr_only(self):
        self.assertIn("branches: [ci/issue-50-hosted-shadow]", self.stripped)
        self.assertIn("branches: [main]", self.stripped)
        for forbidden in ("workflow_dispatch", "schedule", "merge_group"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.stripped)

    def test_permissions_minimal_no_write_no_secrets(self):
        self.assertIn("permissions: {}", self.stripped)
        self.assertNotIn("secrets: inherit", self.text)
        self.assertNotIn("${{ secrets.", self.text)
        self.assertNotRegex(self.text, r"(?m)^\s*secrets:\s*$")
        self.assertNotIn("id-token", self.text)
        self.assertIn("persist-credentials: false", self.text)

    def test_concurrency_is_independent_and_pr_only_cancel(self):
        self.assertIn("ci-shadow-v1-${{", self.stripped)
        self.assertIn("github.run_id", self.stripped)
        self.assertIn("cancel-in-progress: ${{ github.event_name == 'pull_request' }}",
                      self.stripped)
        self.assertNotIn("${{ github.workflow }}", self.stripped)

    def test_local_jobs_are_hosted_ubuntu_24_04(self):
        runs_on = re.findall(r"(?m)^\s*runs-on:\s*(\S+)\s*$", self.stripped)
        self.assertTrue(runs_on, "应存在本地 job 的 runs-on")
        self.assertEqual(set(runs_on), {"ubuntu-24.04"})

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

    def test_no_caller_supplied_shell_or_jobs_input(self):
        self.assertIn("scripts/ci/changed_files.py", self.stripped)
        self.assertNotIn("workflow_call", self.stripped)
        self.assertIn("source_sha: ${{ github.sha }}", self.stripped)

    def test_required_and_optional_are_disjoint_and_build_is_optional(self):
        self.assertIn("required_checks_json: '[\"changes\",\"plan\"]'", self.stripped)
        self.assertIn("optional_checks_json: '[\"build\"]'", self.stripped)
        # 重型 build 为 optional：仅 docs-only 允许跳过。
        self.assertIn("needs.plan.outputs.classification != 'docs-only'", self.stripped)
        self.assertIn("needs.plan.result == 'success'", self.stripped)

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

    def test_no_artifact_and_no_cache(self):
        for forbidden in ("upload-artifact", "download-artifact", "actions/cache",
                          "ccache"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.stripped)

    def test_preserves_gate_commands(self):
        # 影子构建 job 的判据命令（与 unit_test.yml 逐项一致的见 Coupling 测试）。
        for needle in (
            "-DCMAKE_BUILD_TYPE=Release -DNEED_TEST=ON -DNEED_BENCHMARK=ON",
            'cmake --build . --parallel "$JOBS"',
            "ctest --timeout 60 --output-on-failure",
            "90s bin/unit_test/Test_smoke --log_level=test_suite",
            "120s bin/unit_test/Test_reliability --log_level=test_suite",
            "scripts/ci/prepare_boost.sh",
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
# 影子 workflow 结构契约（需 PyYAML）
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
class WorkflowStructureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert yaml is not None
        cls.wf = yaml.load(raw(SHADOW), Loader=_loader())
        cls.text = raw(SHADOW)

    def test_toplevel_shape_and_jobs(self):
        self.assertEqual(self.wf["name"], "ci-shadow-v1")
        self.assertEqual(
            set(self.wf), {"name", "on", "permissions", "concurrency", "env", "jobs"}
        )
        self.assertEqual(set(self.wf["jobs"]), {"changes", "plan", "build", "result"})

    def test_triggers(self):
        on = self.wf["on"]
        self.assertEqual(set(on), {"push", "pull_request"})
        self.assertEqual(on["push"]["branches"], ["ci/issue-50-hosted-shadow"])
        self.assertEqual(on["pull_request"]["branches"], ["main"])

    def test_permissions_minimal(self):
        self.assertEqual(self.wf["permissions"], {})
        for name, job in self.wf["jobs"].items():
            with self.subTest(job=name):
                self.assertNotIn("write", str(job.get("permissions", {})))
                if "uses" not in job:
                    self.assertEqual(job.get("permissions"), {"contents": "read"})

    def test_reusable_jobs_have_no_runs_on(self):
        for name, job in self.wf["jobs"].items():
            if "uses" in job:
                self.assertNotIn("runs-on", job, f"{name} 为 reusable 调用")
                self.assertEqual(job["uses"], CALLEE)
                self.assertEqual(job["with"]["profile"], "hosted")
                self.assertIn("github.sha", job["with"]["source_sha"])

    def test_local_jobs_runs_on_hosted(self):
        for name, job in self.wf["jobs"].items():
            if "uses" in job:
                continue
            with self.subTest(job=name):
                self.assertEqual(job["runs-on"], "ubuntu-24.04")

    def test_required_optional_disjoint(self):
        plan = next(
            j for j in self.wf["jobs"].values()
            if "uses" in j and j.get("needs") == ["changes"]
        )
        required = json.loads(plan["with"]["required_checks_json"])
        optional = json.loads(plan["with"]["optional_checks_json"])
        self.assertTrue(required)
        self.assertFalse(set(required) & set(optional))
        self.assertEqual(optional, ["build"])

    def test_result_aggregates_all(self):
        result = self.wf["jobs"]["result"]
        self.assertEqual(result["if"], "${{ always() }}")
        self.assertEqual(set(result["needs"]), {"changes", "plan", "build"})


# --------------------------------------------------------------------------- #
# 与现役 unit_test.yml 的直接耦合（防漂移）
# --------------------------------------------------------------------------- #
class UnitTestYmlCouplingTests(unittest.TestCase):
    """影子 build job 与现役 Layer 1 判据必须逐项一致；本仓不改 unit_test.yml。"""

    @classmethod
    def setUpClass(cls):
        cls.shadow = body(SHADOW)
        cls.unit = body(UNIT_YML)

    def _both(self, needle):
        self.assertIn(needle, self.unit, "现役 unit_test.yml 缺该判据（可能漂移）")
        self.assertIn(needle, self.shadow, "影子缺该判据（未对齐现役）")

    def test_build_options_match(self):
        for needle in (
            "cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEED_TEST=ON -DNEED_BENCHMARK=ON",
            'cmake --build . --parallel "$JOBS"',
            "JOBS: 3",
        ):
            with self.subTest(needle=needle):
                self._both(needle)

    def test_tool_self_tests_match(self):
        for needle in (
            "python3 scripts/test_release_gate.py",
            "python3 -m unittest discover -s scripts/ci -p 'test_*.py'",
        ):
            with self.subTest(needle=needle):
                self._both(needle)

    def test_ctest_criteria_match(self):
        for needle in (
            "ctest --timeout 60 --output-on-failure",
            "for i in 1 2 3; do",
            'echo "ctest attempt $i failed, retrying..."',
        ):
            with self.subTest(needle=needle):
                self._both(needle)

    def test_smoke_and_reliability_match(self):
        for needle in (
            "timeout --signal=TERM --kill-after=10s 90s bin/unit_test/Test_smoke --log_level=test_suite",
            'echo "Test_smoke attempt $i failed, retrying..."',
            "timeout --signal=TERM --kill-after=10s 120s bin/unit_test/Test_reliability --log_level=test_suite",
            'echo "Test_reliability attempt $i failed, retrying..."',
        ):
            with self.subTest(needle=needle):
                self._both(needle)

    def test_shadow_does_not_add_retries(self):
        # 不扩重试：现役 3 次有界重试，影子同为 3 次，不多不少。
        self.assertEqual(self.shadow.count("for i in 1 2 3; do"),
                         self.unit.count("for i in 1 2 3; do"))

    def test_shadow_excludes_perf_release_memcheck(self):
        # 影子不带性能比较 / 基线读取写入 / 发布 / memcheck。
        for forbidden in (
            "ci_perf_check.py", "perf-baseline", "tests/baselines",
            "run_memcheck", "record_baseline", "release_gate.py publish",
            "unified_stress",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.shadow)


# --------------------------------------------------------------------------- #
# 分类路由冒烟：真实复用 framework callee 逻辑（不复制契约）
# --------------------------------------------------------------------------- #
SHARED = os.environ.get("BBT_CI_SHARED_DIR", "")


def _shadow_payload(changed_files_json, classifier_status="ok"):
    return {
        "repo": "yqm-307/bbtools-coroutine",
        "source_sha": "a" * 40,
        "profile": "hosted",
        "concurrency": "2",
        "timeout_minutes": "60",
        "event_json": '{"name":"pull_request","base_ref":"main"}',
        "changed_files_json": changed_files_json,
        "required_checks_json": '["changes","plan"]',
        "optional_checks_json": '["build"]',
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
            json.dump(_shadow_payload(changed, status), handle)
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

    def test_docs_only_routing(self):
        proc = self._classify('["agent-docs/ci-shadow-v1.md"]')
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
        proc = self._classify('["agent-docs/ci-shadow-v1.md"]', status="failed")
        self.assertEqual(json.loads(proc.stdout)["classification"], "unknown")
        self.assertEqual(
            json.loads(self._classify("[]").stdout)["classification"], "unknown"
        )

    def test_docs_only_allows_build_skip_and_is_success(self):
        plan = self._plan_for('["agent-docs/ci-shadow-v1.md"]')
        proc = self._evaluate(
            plan, {"changes": "success", "plan": "success", "build": "skipped"}
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(json.loads(proc.stdout)["verdict"], "success")

    def test_code_build_failure_is_failure(self):
        plan = self._plan_for('["bbt/x.cc"]')
        proc = self._evaluate(
            plan, {"changes": "success", "plan": "success", "build": "failure"}
        )
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(json.loads(proc.stdout)["verdict"], "failure")

    def test_required_skipped_is_failure(self):
        plan = self._plan_for('["bbt/x.cc"]')
        proc = self._evaluate(
            plan, {"changes": "success", "plan": "skipped", "build": "skipped"}
        )
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(json.loads(proc.stdout)["verdict"], "failure")

    def test_unknown_classification_forbids_build_skip(self):
        plan = self._plan_for("[]", status="failed")
        proc = self._evaluate(
            plan, {"changes": "success", "plan": "success", "build": "skipped"}
        )
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(json.loads(proc.stdout)["verdict"], "failure")

    def test_required_cancelled_is_failure(self):
        plan = self._plan_for('["bbt/x.cc"]')
        proc = self._evaluate(
            plan, {"changes": "cancelled", "plan": "success", "build": "success"}
        )
        self.assertEqual(proc.returncode, 1)

    def test_code_build_success_is_success(self):
        plan = self._plan_for('["bbt/x.cc"]')
        proc = self._evaluate(
            plan, {"changes": "success", "plan": "success", "build": "success"}
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
