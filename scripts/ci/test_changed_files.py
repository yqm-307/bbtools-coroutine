#!/usr/bin/env python3
"""changed_files.py 的分类规则、perf 触发位与 result.py 的结果契约单元测试（仅标准库）。

分类用例集覆盖：CMakeLists.txt、*.cmake、*.py、.github/workflows/** 判 code；
纯 md/docs 判 docs-only；混合判 code；空集/非 ok 状态判 unknown。
perf 触发位覆盖：bbt/**、benchmarks/**（含旧名 benchmark_test/** 兼容映射）、
perf 脚本集合、根 CMakeLists.txt 为真；tests/docs/examples/scripts 其他文件为假；
unknown 状态恒真（保守执行）。

运行：
  PYTHONDONTWRITEBYTECODE=1 python3 -m unittest -v scripts/ci/test_changed_files.py
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import changed_files  # noqa: E402
import result as result_mod  # noqa: E402

ENV = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")


class ClassifyPathTests(unittest.TestCase):
    """单路径分类：doc | code。"""

    def test_cmake_lists_txt_is_code(self):
        # 构建入口绝不允许被 .txt 后缀带成 docs-only。
        for path in ("CMakeLists.txt", "bbt/CMakeLists.txt",
                     "tests/CMakeLists.txt", "benchmarks/CMakeLists.txt"):
            with self.subTest(path=path):
                self.assertEqual(changed_files.classify_path(path), "code")

    def test_cmake_suffix_is_code(self):
        for path in ("cmake/NetworkOnly.cmake", "cmake/x.CMAKE"):
            with self.subTest(path=path):
                self.assertEqual(changed_files.classify_path(path), "code")

    def test_python_is_code(self):
        for path in ("scripts/ci/changed_files.py", "scripts/ci_perf_check.py",
                     "tests/helper.py"):
            with self.subTest(path=path):
                self.assertEqual(changed_files.classify_path(path), "code")

    def test_workflows_are_code(self):
        for path in (".github/workflows/unit_test.yml", ".github/workflows/x.yaml"):
            with self.subTest(path=path):
                self.assertEqual(changed_files.classify_path(path), "code")

    def test_markdown_and_docs_are_doc(self):
        for path in ("README.md", "docs/guides/ci.md", "docs/architecture/x.md",
                     "AGENTS.md", "CHANGELOG.txt", "docs/pic.png", "LICENSE",
                     "agent-docs/any.md",  # 历史目录名，仅作 .md 判 doc 的路径形状用例
                     ):
            with self.subTest(path=path):
                self.assertEqual(changed_files.classify_path(path), "doc")

    def test_source_and_unknown_suffix_are_code(self):
        for path in ("bbt/coroutine/Scheduler.cc", "bbt/core/x.hpp",
                     "scripts/run_parallel_stress.sh", "build.sh",
                     "tests/coroutine/Test_coroutine.cc", "examples/echo_server.cc"):
            with self.subTest(path=path):
                self.assertEqual(changed_files.classify_path(path), "code")


class ClassifyChangesTests(unittest.TestCase):
    """变更集分类：docs-only | code | unknown。"""

    def test_pure_docs_set_is_docs_only(self):
        self.assertEqual(
            changed_files.classify_changes(["docs/guides/ci.md", "README.md"]),
            "docs-only",
        )

    def test_empty_set_is_unknown(self):
        self.assertEqual(changed_files.classify_changes([]), "unknown")

    def test_mixed_set_is_code(self):
        # md + CMakeLists.txt 混合必须判 code（防止构建入口改动被文档带偏）。
        self.assertEqual(
            changed_files.classify_changes(["docs/a.md", "CMakeLists.txt"]),
            "code",
        )
        self.assertEqual(
            changed_files.classify_changes(["README.md", "scripts/ci/result.py"]),
            "code",
        )

    def test_status_not_ok_is_unknown(self):
        for status in ("failed", "unknown", "over-budget", ""):
            with self.subTest(status=status):
                self.assertEqual(
                    changed_files.classify_status(status, ["README.md"]), "unknown"
                )
        self.assertEqual(
            changed_files.classify_status("ok", ["README.md"]), "docs-only"
        )


class PerfTriggerTests(unittest.TestCase):
    """perf 触发位：仅 bbt/**、benchmarks/**（含 benchmark_test/ 旧名兼容映射）、
    perf 脚本集合、根 CMakeLists.txt 改动为真；其余路径与 unknown 状态语义正确。"""

    def test_bbt_source_triggers_perf(self):
        for path in ("bbt/coroutine/Scheduler.cc", "bbt/core/thread/Mutex.hpp",
                     "bbt/pollevent/EventLoop.cc"):
            with self.subTest(path=path):
                self.assertTrue(changed_files.triggers_perf(path))

    def test_benchmarks_triggers_perf(self):
        for path in ("benchmarks/unified_stress.cc",
                     "benchmarks/fatigue_metrics.h",
                     "benchmarks/CMakeLists.txt"):
            with self.subTest(path=path):
                self.assertTrue(changed_files.triggers_perf(path))

    def test_old_benchmark_test_maps_to_new_trigger(self):
        # co-rename 后历史 diff 中的 benchmark_test/** 必须折回 benchmarks/** 触发集。
        for path in ("benchmark_test/unified_stress.cc", "benchmark_test/CMakeLists.txt"):
            with self.subTest(path=path):
                self.assertTrue(changed_files.triggers_perf(path))

    def test_perf_scripts_trigger(self):
        for path in ("scripts/ci_perf_check.py", "scripts/ci/perf_contract.py",
                     "scripts/ci/ci_common.py", "scripts/ci/prepare_boost.sh",
                     "scripts/record_baseline.py", "scripts/run_fatigue.py",
                     "scripts/run_parallel_stress.sh"):
            with self.subTest(path=path):
                self.assertTrue(changed_files.triggers_perf(path))

    def test_root_cmake_lists_triggers_perf(self):
        self.assertTrue(changed_files.triggers_perf("CMakeLists.txt"))

    def test_non_perf_paths_do_not_trigger(self):
        # 测试、文档、示例与其他脚本不影响测量结论，不得触发 perf。
        for path in ("tests/coroutine/Test_coroutine.cc", "tests/CMakeLists.txt",
                     "unit_test/legacy_Test_x.cc", "docs/guides/ci.md",
                     "README.md", "examples/echo_server.cc",
                     "examples/echo/x.cc", "scripts/ci/run_smoke.py",
                     "scripts/ci/run_soak.py", "scripts/ci/run_memcheck.py",
                     "scripts/ci/test_perf_contract.py",
                     "scripts/test_release_gate.py",
                     "scripts/stack_budget.py",
                     ".github/workflows/unit_test.yml",
                     ".github/workflows/memery_test_info.yml",
                     ):
            with self.subTest(path=path):
                self.assertFalse(changed_files.triggers_perf(path))

    def test_perf_status_unknown_is_true(self):
        # 分类未知/失败/超预算一律触发 perf，保守执行。
        for status in ("failed", "unknown", "over-budget", ""):
            with self.subTest(status=status):
                self.assertTrue(changed_files.perf_status(status, ["README.md"]))
        # 空集（diff 空/无法确定）也恒真。
        self.assertTrue(changed_files.perf_status("ok", []))

    def test_perf_status_ok_by_content(self):
        self.assertTrue(changed_files.perf_status("ok", ["bbt/x.cc"]))
        self.assertTrue(changed_files.perf_status(
            "ok", ["docs/a.md", "benchmarks/x.cc"]))  # 改名后混合集仍触发
        self.assertFalse(changed_files.perf_status("ok", ["docs/a.md"]))
        self.assertFalse(changed_files.perf_status(
            "ok", ["tests/core/Test_x.cc", "docs/a.md"]))


class ChangedFilesRoutingTests(unittest.TestCase):
    """真实 git diff → GITHUB_OUTPUT 的路由（沿用既有口径，新增 classification/perf 输出）。"""

    def _git(self, repo, *args):
        return subprocess.run(["git", "-C", repo, *args], env=ENV,
                              capture_output=True, text=True, check=False)

    def _init_repo(self, repo):
        self._git(repo, "init", "-q", "-b", "main")
        self._git(repo, "config", "user.email", "t@t")
        self._git(repo, "config", "user.name", "t")
        self._git(repo, "config", "commit.gpgsign", "false")

    def _commit(self, repo, relpath, content, message):
        full = os.path.join(repo, relpath)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8") as handle:
            handle.write(content)
        self._git(repo, "add", relpath)
        self._git(repo, "commit", "-q", "-m", message)
        return self._git(repo, "rev-parse", "HEAD").stdout.strip()

    def _run(self, repo, env_extra):
        out_path = os.path.join(repo, ".ci-output")
        env = dict(ENV, GITHUB_OUTPUT=out_path, **env_extra)
        proc = subprocess.run([sys.executable,
                               os.path.join(HERE, "changed_files.py")],
                              env=env, cwd=repo, capture_output=True, text=True)
        with open(out_path, encoding="utf-8") as handle:
            values = dict(line.split("=", 1)
                          for line in handle.read().splitlines() if "=" in line)
        return proc, values

    def test_push_code_change_classified_code_and_perf(self):
        with tempfile.TemporaryDirectory() as repo:
            self._init_repo(repo)
            first = self._commit(repo, "docs/a.md", "a", "docs")
            second = self._commit(repo, "bbt/coroutine/x.cc", "x", "code")
            proc, values = self._run(
                repo, {"EVENT_NAME": "push", "BEFORE": first, "SHA": second,
                       "REF_NAME": "main"})
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(values["classifier_status"], "ok")
            self.assertEqual(values["classification"], "code")
            self.assertEqual(values["perf"], "true")
            self.assertEqual(json.loads(values["changed_files_json"]),
                             ["bbt/coroutine/x.cc"])

    def test_push_docs_change_classified_docs_only_no_perf(self):
        with tempfile.TemporaryDirectory() as repo:
            self._init_repo(repo)
            first = self._commit(repo, "bbt/x.cc", "x", "code")
            second = self._commit(repo, "docs/guides/ci.md", "d", "docs")
            proc, values = self._run(
                repo, {"EVENT_NAME": "push", "BEFORE": first, "SHA": second,
                       "REF_NAME": "main"})
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(values["classification"], "docs-only")
            self.assertEqual(values["perf"], "false")

    def test_push_tests_change_is_code_no_perf(self):
        # tests 改动是 code 但不触发 perf（不影响测量结果）。
        with tempfile.TemporaryDirectory() as repo:
            self._init_repo(repo)
            first = self._commit(repo, "docs/a.md", "a", "base")
            second = self._commit(repo, "tests/Test_x.cc", "t", "test")
            proc, values = self._run(
                repo, {"EVENT_NAME": "push", "BEFORE": first, "SHA": second,
                       "REF_NAME": "main"})
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(values["classification"], "code")
            self.assertEqual(values["perf"], "false")

    def test_pr_cmake_change_is_code_and_perf(self):
        with tempfile.TemporaryDirectory() as repo:
            self._init_repo(repo)
            base = self._commit(repo, "docs/a.md", "a", "base")
            self._git(repo, "update-ref", "refs/remotes/origin/main", base)
            self._git(repo, "checkout", "-q", "-b", "feat")
            head = self._commit(repo, "CMakeLists.txt", "c", "cmake")
            proc, values = self._run(
                repo, {"EVENT_NAME": "pull_request", "BASE_REF": "main", "SHA": head})
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(json.loads(values["changed_files_json"]),
                             ["CMakeLists.txt"])
            self.assertEqual(values["classification"], "code")
            self.assertEqual(values["perf"], "true")

    def test_pr_benchmarks_rename_change_triggers_perf(self):
        # 改名后 benchmarks/** 经兼容映射仍触发 perf。
        with tempfile.TemporaryDirectory() as repo:
            self._init_repo(repo)
            base = self._commit(repo, "docs/a.md", "a", "base")
            self._git(repo, "update-ref", "refs/remotes/origin/main", base)
            self._git(repo, "checkout", "-q", "-b", "feat")
            head = self._commit(repo, "benchmarks/unified_stress.cc", "s", "bench")
            proc, values = self._run(
                repo, {"EVENT_NAME": "pull_request", "BASE_REF": "main", "SHA": head})
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(values["classification"], "code")
            self.assertEqual(values["perf"], "true")

    def test_unknown_range_is_conservative_unknown_and_perf(self):
        with tempfile.TemporaryDirectory() as repo:
            self._init_repo(repo)
            first = self._commit(repo, "bbt/x.cc", "x", "one")
            proc, values = self._run(
                repo, {"EVENT_NAME": "push", "BEFORE": "f" * 40, "SHA": first})
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(values["changed_files_json"], "[]")
            self.assertEqual(values["classifier_status"], "unknown")
            self.assertEqual(values["classification"], "unknown")
            self.assertEqual(values["perf"], "true")


class ResultContractTests(unittest.TestCase):
    """result.py：必跑集失败闭合、expected_skips 显式放行、防全跳过变绿。"""

    def _eval(self, results, required, optional=(), expected_skips=()):
        return result_mod.evaluate(results, required, list(optional),
                                   list(expected_skips))

    def test_all_success_is_success(self):
        out = self._eval({"changes": "success", "build": "success", "test": "success"},
                         ["changes"], ["build", "test"])
        self.assertEqual(out["verdict"], "success")
        self.assertEqual(out["failed"], [])

    def test_expected_skip_allows_optional_skipped(self):
        # 声明 expected_skips 的 optional 跳过放行（docs-only→build-test、
        # perf=false→perf-regression 均由调用方显式声明）。
        out = self._eval({"changes": "success", "build": "skipped", "test": "skipped"},
                         ["changes"], ["build", "test"], ["build", "test"])
        self.assertEqual(out["verdict"], "success")
        self.assertEqual(sorted(out["skipped"]), ["build", "test"])

    def test_optional_skipped_without_expected_skip_is_failure(self):
        out = self._eval({"changes": "success", "build": "skipped", "test": "success"},
                         ["changes"], ["build", "test"], ["test"])
        self.assertEqual(out["verdict"], "failure")
        self.assertEqual(out["failed"], ["build"])

    def test_expected_skip_is_not_a_blanket(self):
        # 只声明 build 可跳过不能顺带放行 test 的跳过。
        out = self._eval({"changes": "success", "build": "skipped", "test": "skipped"},
                         ["changes"], ["build", "test"], ["build"])
        self.assertEqual(out["verdict"], "failure")
        self.assertEqual(out["failed"], ["test"])

    def test_no_classification_means_no_implicit_skip(self):
        # result.py 不再消费 classification：任何未声明的 optional 跳过都判红。
        out = self._eval({"changes": "success", "build": "skipped"},
                         ["changes"], ["build"], [])
        self.assertEqual(out["verdict"], "failure")

    def test_required_non_success_is_failure(self):
        for status in ("failure", "cancelled", "skipped"):
            with self.subTest(status=status):
                out = self._eval({"changes": status, "build": "success"},
                                 ["changes"], ["build"])
                self.assertEqual(out["verdict"], "failure")

    def test_required_missing_is_failure(self):
        out = self._eval({"build": "success"}, ["changes"], ["build"])
        self.assertEqual(out["verdict"], "failure")
        self.assertEqual(out["failed"], ["changes"])

    def test_optional_failure_is_failure_even_when_skippable(self):
        out = self._eval({"changes": "success", "build": "failure"},
                         ["changes"], ["build"], ["build"])
        self.assertEqual(out["verdict"], "failure")

    def test_rejects(self):
        with self.assertRaises(result_mod.ContractError):
            self._eval({}, ["changes"], [])
        with self.assertRaises(result_mod.ContractError):
            self._eval({"changes": "success"}, [], [])  # 空 required 拒绝
        with self.assertRaises(result_mod.ContractError):
            self._eval({"changes": "success", "other": "success"},
                       ["changes"], [])  # 集合外 job 拒绝
        with self.assertRaises(result_mod.ContractError):
            self._eval({"changes": "bogus"}, ["changes"], [])  # 非法状态拒绝
        with self.assertRaises(result_mod.ContractError):
            self._eval({"a": "success"}, ["a"], ["a"])  # required/optional 重叠
        with self.assertRaises(result_mod.ContractError):
            # expected_skips 必须是 optional 子集：给 required 或未知 job 放行 → 拒绝
            self._eval({"a": "success"}, ["a"], [], ["a"])
        with self.assertRaises(result_mod.ContractError):
            self._eval({"a": "success", "b": "skipped"}, ["a"], ["b"], ["b", "x"])

    def test_cli_exit_codes(self):
        script = os.path.join(HERE, "result.py")

        def run(payload):
            return subprocess.run([sys.executable, script], input=payload,
                                  capture_output=True, text=True, env=ENV)

        ok = run(json.dumps({"results": {"changes": "success"},
                             "required": ["changes"]}))
        self.assertEqual(ok.returncode, 0, ok.stdout + ok.stderr)
        self.assertEqual(json.loads(ok.stdout)["verdict"], "success")

        bad = run(json.dumps({"results": {"changes": "failure"},
                              "required": ["changes"]}))
        self.assertEqual(bad.returncode, 1)
        self.assertEqual(json.loads(bad.stdout)["verdict"], "failure")

        rejected = run("not json")
        self.assertEqual(rejected.returncode, 3)
        self.assertFalse(json.loads(rejected.stdout)["ok"])

        missing = run(json.dumps({"results": {}}))
        self.assertEqual(missing.returncode, 3)

        skip_not_optional = run(json.dumps(
            {"results": {"a": "success"}, "required": ["a"],
             "expected_skips": ["a"]}))
        self.assertEqual(skip_not_optional.returncode, 3)


if __name__ == "__main__":
    unittest.main()
