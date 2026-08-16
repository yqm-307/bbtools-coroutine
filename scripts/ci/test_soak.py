#!/usr/bin/env python3
"""Soak Harness 契约测试；所有长时路径使用短 fake 子进程。"""

from __future__ import annotations

import json
import os
import signal
import tempfile
import unittest
from argparse import Namespace
from pathlib import Path
from unittest import mock

import run_soak


class SoakContractTest(unittest.TestCase):
    def test_parse_metric_line(self):
        metric = run_soak.parse_metric_line('FATIGUE_METRIC:{"name":"comutex","ops_total":10,"errors":0}')
        self.assertEqual(metric["name"], "comutex")
        self.assertIsNone(run_soak.parse_metric_line("not metric"))

    def test_stall_after_two_unchanged_samples(self):
        self.assertTrue(run_soak.has_stall([10, 10, 10], 2))
        self.assertFalse(run_soak.has_stall([10, 10, 11], 2))

    def test_all_modules_required(self):
        result = run_soak.evaluate_function_metrics({"comutex": {"ops_total": 1}}, run_soak.REQUIRED_MODULES)
        self.assertEqual(result.status, "METRIC_MISSING")

    def test_resource_thresholds_use_peak_input(self):
        self.assertEqual(run_soak.evaluate_resource(140, 5, 128, 256).status, "WARN")
        self.assertEqual(run_soak.evaluate_resource(260, 5, 128, 256).status, "FAIL")

    def test_formal_cli_names(self):
        args = run_soak.parse_args([
            "--build-dir", "build-x", "--duration-seconds", "30", "--threads", "1",
            "--resource-interval-seconds", "2", "--metric-interval-seconds", "5",
            "--report-dir", "tests/reports/soak",
        ])
        self.assertEqual(args.build_dir, Path("build-x"))
        self.assertEqual(args.duration_seconds, 30)
        self.assertEqual(args.resource_interval_seconds, 2)

    def test_report_path_rejects_escape_and_avoids_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            allowed = root / "tests/reports/soak"
            with self.assertRaises(ValueError):
                run_soak.create_run_dir(root / "elsewhere", root)
            with mock.patch.object(run_soak, "_compact_utc", return_value="20260101T000000Z"):
                first = run_soak.create_run_dir(allowed, root)
                second = run_soak.create_run_dir(allowed, root)
            self.assertNotEqual(first, second)
            self.assertTrue(first.is_dir() and second.is_dir())

    def test_resource_summary_uses_max_and_observes_trend(self):
        summary = run_soak._resource_summary([
            {"rss_mib": 10, "cpu_pct": 2, "threads": 2},
            {"rss_mib": 30, "cpu_pct": 90, "threads": 5},
            {"rss_mib": 20, "cpu_pct": 3, "threads": 3},
        ])
        self.assertEqual(summary["max_rss_mib"], 30)
        self.assertEqual(summary["max_cpu_pct"], 90)
        self.assertEqual(summary["max_threads"], 5)
        self.assertFalse(summary["rss_monotonic_growth_observed"])

    def test_stdout_errors_are_scanned_after_ansi_removed(self):
        hits = run_soak.scan_error_hits("\x1b[31mERROR boom\x1b[0m")
        self.assertEqual(hits[0]["line"], "ERROR boom")


class SoakIntegrationTest(unittest.TestCase):
    def _args(self, build: Path, report: Path, duration: float = 0.8, grace: float = 0.2) -> Namespace:
        return run_soak.parse_args([
            "--build-dir", str(build), "--duration-seconds", str(duration), "--threads", "1",
            "--resource-interval-seconds", "0.1", "--metric-interval-seconds", "1",
            "--report-dir", str(report), "--sigterm-grace-seconds", str(grace),
        ])

    def _write_binary(self, root: Path, body: str) -> Path:
        binary = root / "bin/benchmark_test/unified_stress"
        binary.parent.mkdir(parents=True)
        binary.write_text("#!/usr/bin/env python3\n" + body, encoding="utf-8")
        binary.chmod(0o755)
        return binary

    def test_success_creates_exact_five_reports(self):
        body = (
            "import json,signal,sys,time\n"
            "signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))\n"
            f"mods={run_soak.REQUIRED_MODULES!r}\n"
            "end=time.monotonic()+5\n"
            "i=0\n"
            "while time.monotonic()<end:\n"
            "  i+=1\n"
            "  for m in mods: print('FATIGUE_METRIC:'+json.dumps({'name':m,'ops_total':i,'errors':0}),flush=True)\n"
            "  time.sleep(.1)\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = root / "build"
            self._write_binary(build, body)
            report_root = root / "tests/reports/soak"
            args = self._args(build, report_root)
            binary = run_soak.validate_args(args, root)
            run_dir = run_soak.create_run_dir(report_root, root)
            report, samples, metrics, raw = run_soak.run_soak(args, binary, run_dir)
            run_soak.write_reports(report, samples, metrics, raw, run_dir)
            self.assertEqual(report["exit_code"], 0)
            self.assertEqual(sorted(path.name for path in run_dir.iterdir()), sorted(run_soak.REPORT_FILES))
            summary = json.loads((run_dir / "summary.json").read_text())
            self.assertTrue(summary["process"]["completed_target_duration"])

    def test_early_exit_is_failure(self):
        body = "print('done',flush=True)\n"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = root / "build"
            self._write_binary(build, body)
            report_root = root / "tests/reports/soak"
            args = self._args(build, report_root, duration=1)
            binary = run_soak.validate_args(args, root)
            run_dir = run_soak.create_run_dir(report_root, root)
            report, *_ = run_soak.run_soak(args, binary, run_dir)
            self.assertEqual(report["process"]["category"], "early_exit")
            self.assertEqual(report["exit_code"], 1)

    def test_forced_kill_is_failure(self):
        body = "import signal,time\nsignal.signal(signal.SIGTERM, signal.SIG_IGN)\ntime.sleep(10)\n"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build = root / "build"
            self._write_binary(build, body)
            report_root = root / "tests/reports/soak"
            args = self._args(build, report_root, duration=.3, grace=.1)
            binary = run_soak.validate_args(args, root)
            run_dir = run_soak.create_run_dir(report_root, root)
            report, *_ = run_soak.run_soak(args, binary, run_dir)
            self.assertEqual(report["process"]["category"], "forced_kill")
            self.assertEqual(report["exit_code"], 1)

    def test_invalid_arguments(self):
        args = run_soak.parse_args(["--duration-seconds", "0"])
        with self.assertRaises(ValueError):
            run_soak.validate_args(args, Path.cwd())


if __name__ == "__main__":
    unittest.main()
