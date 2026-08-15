#!/usr/bin/env python3
"""Jenkins 测试公共契约的最小测试骨架。

覆盖 classify_process_result / write_json_atomic（任务 1 核心契约），
以及 run_command / utc_timestamp / ensure_dir / sanitized_env_summary
（简报要求的配套公共函数）的基础行为。
"""

import json
import tempfile
import unittest
from datetime import datetime
from pathlib import Path

from ci_common import (
    classify_process_result,
    ensure_dir,
    run_command,
    sanitized_env_summary,
    utc_timestamp,
    write_json_atomic,
)


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

    def test_run_command_returns_rc_stdout_stderr(self):
        rc, out, err = run_command(["echo", "hello"])
        self.assertEqual(rc, 0)
        self.assertEqual(out.strip(), "hello")
        self.assertEqual(err, "")

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


if __name__ == "__main__":
    unittest.main()
