#!/usr/bin/env python3
"""Jenkins 测试公共契约的最小测试骨架。

覆盖 classify_process_result / write_json_atomic（任务 1 核心契约）、
run_command / utc_timestamp / ensure_dir / sanitized_env_summary
（简报要求的配套公共函数）、run_command 超时与优雅终止、dump 文件
扫描分类（任务 3 Smoke Harness 依赖的错误分类契约）。

run_command 从裸 tuple 演进为 CommandResult 对象（含 category 字段），
超时不再抛 TimeoutExpired，而是按 SIGTERM -> SIGKILL 序列终止并归类
为 "timeout"：调用方因此能统一处理正常/失败/崩溃/超时四种结果，
不必为超时写 try/except 分支。
"""

import json
import signal
import sys
import tempfile
import time
import unittest
from datetime import datetime
from pathlib import Path
from unittest import mock

from ci_common import (
    classify_process_result,
    ensure_dir,
    find_dump_files,
    run_command,
    sanitized_env_summary,
    utc_timestamp,
    write_json_atomic,
)

# 简报限定任务 3 的单测文件为 test_ci_common.py，故 run_smoke 的
# validate_args / build_command_list / scan_error_hits / junit 状态
# 等可测逻辑的单测也集中在这里。
import run_smoke


class CommonContractTest(unittest.TestCase):
    def test_classify_timeout(self):
        self.assertEqual(classify_process_result(None, timed_out=True), "timeout")

    def test_classify_signal(self):
        self.assertEqual(classify_process_result(-11, timed_out=False), "crash")

    def test_classify_unknown_exit(self):
        self.assertEqual(classify_process_result(None, timed_out=False), "unknown_exit")

    def test_classify_failure_and_pass(self):
        self.assertEqual(classify_process_result(2, timed_out=False), "failure")
        self.assertEqual(classify_process_result(0, timed_out=False), "pass")

    def test_write_json_creates_parent_and_valid_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "nested" / "result.json"
            write_json_atomic(path, {"status": "ok"})
            self.assertEqual(json.loads(path.read_text()), {"status": "ok"})

    def test_run_command_returns_result_object(self):
        result = run_command(["echo", "hello"])
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.strip(), "hello")
        self.assertEqual(result.stderr, "")
        self.assertEqual(result.category, "pass")
        self.assertFalse(result.timed_out)
        self.assertGreater(result.duration, 0)

    def test_run_command_reports_failure_category(self):
        result = run_command([sys.executable, "-c", "import sys; sys.exit(3)"])
        self.assertEqual(result.category, "failure")
        self.assertEqual(result.returncode, 3)

    def test_run_command_reports_crash_category(self):
        result = run_command([sys.executable, "-c", "import os; os.kill(os.getpid(), 11)"])
        self.assertEqual(result.category, "crash")
        self.assertLess(result.returncode, 0)

    def test_timeout_is_failure(self):
        result = run_command([sys.executable, "-c", "import time; time.sleep(2)"], timeout=0.05)
        self.assertEqual(result.category, "timeout")
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(result.timed_out)

    def test_timeout_sends_sigterm_then_sigkill(self):
        # 进程忽略 SIGTERM 时，harness 必须在宽限后补发 SIGKILL，且仍归类为 timeout。
        code = (
            "import signal, time; "
            "signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(5)"
        )
        start = time.monotonic()
        result = run_command(
            [sys.executable, "-c", code], timeout=0.05, sigterm_grace=0.2
        )
        elapsed = time.monotonic() - start
        self.assertEqual(result.category, "timeout")
        self.assertEqual(result.returncode, -signal.SIGKILL)
        self.assertLess(elapsed, 3.0, "SIGKILL 兜底未生效，进程被拖住")

    def test_utc_timestamp_is_iso_with_tz(self):
        parsed = datetime.fromisoformat(utc_timestamp())
        self.assertIsNotNone(parsed.tzinfo)

    def test_ensure_dir_creates_parents(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "a" / "b"
            self.assertEqual(ensure_dir(path), path)
            self.assertTrue(path.is_dir())

    def test_sanitized_env_summary_redacts_secret_values(self):
        secret = "s3cr3t-value"
        summary = sanitized_env_summary({"CI_TOKEN": secret, "PLAIN": "visible"})
        self.assertEqual(summary["CI_TOKEN"], "<redacted>")
        self.assertNotIn(secret, summary.values())
        self.assertEqual(summary["PLAIN"], "visible")


class DumpScanTest(unittest.TestCase):
    def _make_tree(self):
        tmp = Path(tempfile.mkdtemp())
        (tmp / "build").mkdir()
        (tmp / "build" / "core").write_text("x")
        (tmp / "build" / "core.12345").write_text("x")
        (tmp / "build" / "core.app.12345").write_text("x")  # core_pattern=core.%e.%p 的产物
        (tmp / "src").mkdir()
        (tmp / "src" / "app.dump").write_text("x")
        (tmp / "src" / "crash.mdmp").write_text("x")
        (tmp / "src" / "notes.txt").write_text("x")
        (tmp / "src" / "core_docs.md").write_text("x")  # 名字含 core 但不是 dump 文件
        return tmp

    def test_find_dump_files_detects_core_and_dump(self):
        tmp = self._make_tree()
        hits = [str(p.relative_to(tmp)) for p in find_dump_files([tmp])]
        self.assertEqual(
            sorted(hits),
            [
                "build/core",
                "build/core.12345",
                "build/core.app.12345",
                "src/app.dump",
                "src/crash.mdmp",
            ],
        )

    def test_find_dump_files_skips_missing_dir(self):
        self.assertEqual(find_dump_files([Path("/nonexistent-dir-xyz")]), [])

    def test_find_dump_files_skips_git(self):
        tmp = Path(tempfile.mkdtemp())
        (tmp / ".git").mkdir()
        (tmp / ".git" / "core").write_text("x")
        self.assertEqual(find_dump_files([tmp]), [])


class ValidateArgsTest(unittest.TestCase):
    """validate_args 路径安全单测（审查 #1/#2 补测）。

    build 目录会被整体清空重建：任何指向 source 本身/祖先、文件系统根、
    用户主目录、或主目录下但 source 外的路径都必须拒绝；source 内的
    build 目录（即使绝对路径位于 home 下）必须放行。
    """

    def _args(self, source: Path, build: Path, timeout: float = 900.0):
        return run_smoke.parse_args(
            [
                "--source-dir", str(source),
                "--build-dir", str(build),
                "--timeout-seconds", str(timeout),
            ]
        )

    def test_accepts_build_inside_source(self):
        tmp = Path(tempfile.mkdtemp())
        source = tmp / "repo"
        source.mkdir()
        # build 目录允许尚不存在（resolve 不要求存在）。
        run_smoke.validate_args(self._args(source, source / "build-ci"))

    def test_accepts_build_inside_source_when_home_is_ancestor(self):
        # 开发机常态：仓库在 home 下，build 在仓库内，必须放行。
        fake_home = Path(tempfile.mkdtemp())
        source = fake_home / "work" / "repo"
        source.mkdir(parents=True)
        with mock.patch.object(Path, "home", return_value=fake_home):
            run_smoke.validate_args(self._args(source, source / "build-ci"))

    def test_accepts_build_outside_home_and_source(self):
        # CI 常见布局：source 与 build 都在 home 外（如 /tmp 工作区）。
        tmp = Path(tempfile.mkdtemp())
        source = tmp / "repo"
        source.mkdir()
        run_smoke.validate_args(self._args(source, tmp / "build-ci"))

    def test_rejects_build_equal_source(self):
        tmp = Path(tempfile.mkdtemp())
        source = tmp / "repo"
        source.mkdir()
        with self.assertRaises(SystemExit):
            run_smoke.validate_args(self._args(source, source))

    def test_rejects_build_as_source_ancestor(self):
        tmp = Path(tempfile.mkdtemp())
        source = tmp / "repo"
        source.mkdir()
        with self.assertRaises(SystemExit):
            run_smoke.validate_args(self._args(source, tmp))

    def test_rejects_filesystem_root(self):
        tmp = Path(tempfile.mkdtemp())
        source = tmp / "repo"
        source.mkdir()
        with self.assertRaises(SystemExit):
            run_smoke.validate_args(self._args(source, Path("/")))

    def test_rejects_home_dir(self):
        tmp = Path(tempfile.mkdtemp())
        source = tmp / "repo"
        source.mkdir()
        fake_home = Path(tempfile.mkdtemp())
        with mock.patch.object(Path, "home", return_value=fake_home):
            with self.assertRaises(SystemExit):
                run_smoke.validate_args(self._args(source, fake_home))

    def test_rejects_home_descendant_outside_source(self):
        # 审查 #1 盲区：home 下但 source 外的目录会被 rmtree 整体删除。
        tmp = Path(tempfile.mkdtemp())
        source = tmp / "repo"
        source.mkdir()
        fake_home = Path(tempfile.mkdtemp())
        user_data = fake_home / "Documents"
        user_data.mkdir()
        with mock.patch.object(Path, "home", return_value=fake_home):
            with self.assertRaises(SystemExit):
                run_smoke.validate_args(self._args(source, user_data))

    def test_rejects_non_positive_timeout(self):
        tmp = Path(tempfile.mkdtemp())
        source = tmp / "repo"
        source.mkdir()
        with self.assertRaises(SystemExit):
            run_smoke.validate_args(self._args(source, source / "b", timeout=0))


class JUnitStatusTest(unittest.TestCase):
    """resolve_junit_status 契约（审查 #5 补测）。

    不支持 --output-junit 时 path 必须为 None（不假装生成 XML）；
    支持时必须校验 ctest.xml 是否真实生成。
    """

    def test_unsupported_has_null_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            status = run_smoke.resolve_junit_status(False, Path(tmp))
            self.assertFalse(status["supported"])
            self.assertIsNone(status["path"])
            self.assertFalse(status["generated"])

    def test_supported_with_generated_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "ctest.xml").write_text("<testsuites/>")
            status = run_smoke.resolve_junit_status(True, Path(tmp))
            self.assertTrue(status["supported"])
            self.assertTrue(status["generated"])
            self.assertEqual(status["path"], str(Path(tmp) / "ctest.xml"))

    def test_supported_but_file_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            status = run_smoke.resolve_junit_status(True, Path(tmp))
            self.assertTrue(status["supported"])
            self.assertFalse(status["generated"])


class BuildCommandListTest(unittest.TestCase):
    """build_command_list 契约（审查 #7 补测）：argv 保持数组，JUnit 用绝对路径。"""

    def _args(self):
        tmp = Path(tempfile.mkdtemp())
        source = tmp / "repo"
        source.mkdir()
        args = run_smoke.parse_args(
            [
                "--source-dir", str(source),
                "--build-dir", str(tmp / "build-ci"),
                "--report-dir", str(tmp / "reports" / "smoke" / "t"),
            ]
        )
        return args

    def test_returns_argv_arrays_without_junit(self):
        args = self._args()
        cmds = run_smoke.build_command_list(args, junit_supported=False)
        self.assertEqual(len(cmds), 3)
        self.assertEqual(cmds[0][0], "cmake")
        self.assertIn("-G", cmds[0])
        self.assertNotIn("--output-junit", cmds[2])

    def test_junit_uses_absolute_path(self):
        args = self._args()
        cmds = run_smoke.build_command_list(args, junit_supported=True)
        idx = cmds[2].index("--output-junit")
        self.assertTrue(Path(cmds[2][idx + 1]).is_absolute())


class ScanErrorHitsTest(unittest.TestCase):
    """错误模式扫描（审查 #8 补测）：ANSI 转义剥离后再匹配。"""

    def test_detects_plain_patterns(self):
        text = (
            "Assert failed here\n"
            "AddressSanitizer: heap-use-after-free\n"
            "some ERROR line\n"
        )
        hits = run_smoke.scan_error_hits(text)
        patterns = [h["pattern"] for h in hits]
        self.assertIn("assert", patterns)
        self.assertIn("asan", patterns)
        self.assertIn("error", patterns)

    def test_strips_ansi_before_matching(self):
        # 彩色日志中 \x1b[31m 会截断模式匹配，必须先剥离再扫描。
        text = "\x1b[31mERROR: compile failed\x1b[0m\n\x1b[1mAssert failed\x1b[0m\n"
        hits = run_smoke.scan_error_hits(text)
        patterns = {h["pattern"] for h in hits}
        self.assertIn("error", patterns)
        self.assertIn("assert", patterns)
        for h in hits:
            self.assertNotIn("\x1b", h["line"])

    def test_no_false_positive_on_plain_text(self):
        hits = run_smoke.scan_error_hits("all good\nnothing to see\n")
        self.assertEqual(hits, [])


if __name__ == "__main__":
    unittest.main()
