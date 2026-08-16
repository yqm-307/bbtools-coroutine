#!/usr/bin/env python3
"""统一性能指标 Schema 与基线比较契约的单测（任务 4）。

覆盖：环境指纹比较、模块阈值判定（WARN/FAIL 与 gate 语义）、
指标完整性校验、统一报告 Schema、指纹采集与 Markdown 输出。

compare_module / compare_environment / validate_module_metrics 均返回
带 .status 的结果对象；脚本层按 status 决定退出码。
"""

import contextlib
import io
import json
import os
import platform
import re
import sys
import tempfile
import unittest
import unittest.mock as mock
from pathlib import Path

# 脚本层（scripts/）模块的测试需要父目录进 sys.path：
# ci_perf_check.py / record_baseline.py 位于 scripts/，本文件位于 scripts/ci/。
_SCRIPTS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _SCRIPTS_DIR not in sys.path:
    sys.path.insert(0, _SCRIPTS_DIR)

import ci_perf_check  # noqa: E402
from ci_common import CommandResult  # noqa: E402
import record_baseline  # noqa: E402

from perf_contract import (
    REQUIRED_MODULE_FIELDS,
    REQUIRED_TOP_LEVEL,
    VALID_VERDICTS,
    collect_environment_fingerprint,
    compare_environment,
    compare_module,
    make_report,
    normalize_module_metrics,
    validate_module_metrics,
    validate_report,
    write_markdown_report,
)


class PerfContractTest(unittest.TestCase):
    """任务简报限定的四条核心契约。"""

    def test_incompatible_agent_is_not_comparable(self):
        old = {"environment": {"agent": "perf-a", "compiler": "g++-13"}}
        new = {"environment": {"agent": "perf-b", "compiler": "g++-13"}}
        self.assertEqual(
            compare_environment(old["environment"], new["environment"]).status,
            "NO_COMPARABLE_BASELINE",
        )

    def test_ten_percent_drop_is_warning(self):
        result = compare_module(old_ops=1000, new_ops=890, module="comutex")
        self.assertEqual(result.status, "WARN")

    def test_twenty_percent_drop_is_failure_after_gate_enabled(self):
        result = compare_module(
            old_ops=1000, new_ops=790, module="comutex", gate_enabled=True
        )
        self.assertEqual(result.status, "FAIL")

    def test_missing_metric_is_failure(self):
        self.assertEqual(validate_module_metrics({}).status, "METRIC_INVALID")


class EnvironmentCompareTest(unittest.TestCase):
    def test_same_environment_is_comparable(self):
        env = {"agent": "perf-a", "compiler": "g++-13"}
        self.assertEqual(compare_environment(env, dict(env)).status, "PASS")

    def test_missing_environment_is_not_comparable(self):
        self.assertEqual(compare_environment(None, {}).status, "NO_COMPARABLE_BASELINE")
        self.assertEqual(
            compare_environment({}, {"agent": "x"}).status, "NO_COMPARABLE_BASELINE"
        )

    def test_any_fingerprint_difference_blocks_comparison(self):
        base = {"agent": "perf-a", "cpu_model": "Intel X", "logical_cores": 8}
        other = dict(base)
        other["logical_cores"] = 16
        self.assertEqual(
            compare_environment(base, other).status, "NO_COMPARABLE_BASELINE"
        )


class ModuleCompareTest(unittest.TestCase):
    def test_improvement_is_pass(self):
        self.assertEqual(
            compare_module(old_ops=1000, new_ops=1100, module="comutex").status,
            "PASS",
        )

    def test_gate_disabled_downgrades_fail_to_warn(self):
        # 初期 gate 默认关闭：>20% 退化只报 WARN，不阻塞 PR。
        result = compare_module(old_ops=1000, new_ops=790, module="comutex")
        self.assertEqual(result.status, "WARN")

    def test_zero_old_ops_is_not_comparable(self):
        self.assertEqual(
            compare_module(old_ops=0, new_ops=100, module="comutex").status,
            "NO_COMPARABLE_BASELINE",
        )

    def test_cocond_has_relaxed_threshold(self):
        # CoCond 放宽阈值 30%：-25% 仍 PASS。
        self.assertEqual(
            compare_module(old_ops=1000, new_ops=750, module="cocond").status,
            "PASS",
        )
        # -35% 到 WARN 档。
        self.assertEqual(
            compare_module(old_ops=1000, new_ops=650, module="cocond").status,
            "WARN",
        )

    def test_compare_result_reports_delta(self):
        result = compare_module(old_ops=1000, new_ops=890, module="comutex")
        self.assertAlmostEqual(result.delta_pct, -11.0)
        self.assertEqual(result.old_ops, 1000)
        self.assertEqual(result.new_ops, 890)


class MetricValidationTest(unittest.TestCase):
    def test_accepts_complete_metrics(self):
        metrics = {"ops_total": 100, "ops_per_sec": 10.0, "errors": 0, "elapsed_s": 10.0}
        self.assertEqual(validate_module_metrics(metrics).status, "PASS")

    def test_missing_fields_are_listed(self):
        result = validate_module_metrics({"ops_total": 1})
        self.assertEqual(result.status, "METRIC_INVALID")
        self.assertIn("ops_per_sec", result.missing)
        self.assertIn("errors", result.missing)

    def test_normalize_computes_ops_per_sec_and_drops_ts(self):
        raw = {
            "name": "comutex", "ts": 10.0, "elapsed_s": 10.0,
            "ops_total": 5000, "errors": 0, "lock_ops": 999,
        }
        norm = normalize_module_metrics(raw)
        self.assertEqual(norm["ops_per_sec"], 500.0)
        self.assertNotIn("ts", norm)
        self.assertNotIn("name", norm)
        self.assertEqual(norm["lock_ops"], 999)

    def test_normalize_rejects_missing_fields(self):
        self.assertIsNone(normalize_module_metrics({"ops_total": 1}))

    def test_normalize_rejects_non_positive_elapsed(self):
        raw = {"elapsed_s": 0, "ops_total": 100, "errors": 0}
        self.assertIsNone(normalize_module_metrics(raw))


class ReportSchemaTest(unittest.TestCase):
    def test_required_sets_are_exact(self):
        self.assertEqual(
            REQUIRED_TOP_LEVEL,
            {
                "schema_version", "repository", "commit", "base_commit",
                "environment", "build", "parameters", "modules", "verdict",
            },
        )
        self.assertEqual(
            REQUIRED_MODULE_FIELDS,
            {"ops_total", "ops_per_sec", "errors", "elapsed_s"},
        )
        self.assertEqual(
            VALID_VERDICTS,
            {
                "PASS", "WARN", "FAIL", "UNSTABLE",
                "NO_COMPARABLE_BASELINE", "METRIC_INVALID",
            },
        )

    def test_make_report_produces_valid_report(self):
        report = make_report(
            repository="bbtools-coroutine",
            commit="abc123",
            base_commit=None,
            environment={"agent": "perf-a"},
            build={"type": "Release"},
            parameters={"threads": 2},
            modules={"comutex": {"ops_total": 1, "ops_per_sec": 0.1,
                                 "errors": 0, "elapsed_s": 10.0}},
            verdict="PASS",
        )
        self.assertEqual(validate_report(report).status, "PASS")
        self.assertEqual(report["schema_version"], 1)

    def test_report_rejects_missing_top_level(self):
        self.assertEqual(validate_report({}).status, "METRIC_INVALID")

    def test_report_rejects_unknown_verdict(self):
        report = make_report(
            repository="x", commit="c", base_commit=None, environment={},
            build={}, parameters={}, modules={}, verdict="MAYBE",
        )
        self.assertEqual(validate_report(report).status, "METRIC_INVALID")


class FingerprintTest(unittest.TestCase):
    def test_fingerprint_contains_required_keys(self):
        fp = collect_environment_fingerprint(
            threads=2, modules=["comutex"], durations=[10]
        )
        for key in (
            "agent", "node", "cpu_model", "logical_cores", "mem_total_kb",
            "compiler", "cmake_version", "ninja_version", "build_type",
            "cmake_args", "threads", "modules", "durations",
        ):
            self.assertIn(key, fp)
        self.assertEqual(fp["threads"], 2)
        self.assertEqual(fp["modules"], ["comutex"])
        self.assertEqual(fp["durations"], [10])
        self.assertGreater(fp["logical_cores"], 0)

    def test_fingerprint_difference_blocks_comparison(self):
        base = collect_environment_fingerprint(
            threads=2, modules=["comutex"], durations=[10]
        )
        other = dict(base)
        other["logical_cores"] = 999
        self.assertEqual(compare_environment(base, other).status, "NO_COMPARABLE_BASELINE")


class MarkdownReportTest(unittest.TestCase):
    def test_markdown_mentions_verdict_and_module(self):
        report = make_report(
            repository="bbtools-coroutine",
            commit="abc123",
            base_commit=None,
            environment={"agent": "perf-a", "cpu_model": "X"},
            build={"type": "Release"},
            parameters={"threads": 2},
            modules={
                "comutex": {
                    "ops_total": 1000, "ops_per_sec": 100.0,
                    "errors": 0, "elapsed_s": 10.0, "status": "WARN",
                }
            },
            verdict="WARN",
        )
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "perf.md"
            write_markdown_report(path, report)
            text = path.read_text()
            self.assertIn("WARN", text)
            self.assertIn("comutex", text)


class NormalizeNumericTest(unittest.TestCase):
    """normalize_module_metrics 必须对数值字段做类型校验，防止下游 TypeError。"""

    def test_errors_non_numeric_rejected(self):
        raw = {"elapsed_s": 10.0, "ops_total": 100, "errors": "oops"}
        self.assertIsNone(normalize_module_metrics(raw))

    def test_ops_total_non_numeric_rejected(self):
        raw = {"elapsed_s": 10.0, "ops_total": "1e5", "errors": 0}
        self.assertIsNone(normalize_module_metrics(raw))

    def test_elapsed_non_numeric_rejected(self):
        raw = {"elapsed_s": "10", "ops_total": 100, "errors": 0}
        self.assertIsNone(normalize_module_metrics(raw))

    def test_bool_rejected(self):
        # bool 是 int 子类，不能当作数值指标
        raw = {"elapsed_s": 10.0, "ops_total": 100, "errors": True}
        self.assertIsNone(normalize_module_metrics(raw))

    def test_latency_non_numeric_rejected(self):
        raw = {"elapsed_s": 10.0, "ops_total": 100, "errors": 0,
               "lock_avg_us": "high"}
        self.assertIsNone(normalize_module_metrics(raw))

    def test_numeric_latency_accepted(self):
        raw = {"elapsed_s": 10.0, "ops_total": 100, "errors": 0,
               "lock_avg_us": 1.5, "wlock_avg_us": 0, "cond_avg_us": 2}
        norm = normalize_module_metrics(raw)
        assert norm is not None
        self.assertEqual(norm["lock_avg_us"], 1.5)
        self.assertEqual(norm["wlock_avg_us"], 0)


class GitHubSummaryTest(unittest.TestCase):
    """write_github_summary 的 Overall 必须等于传入的聚合 verdict，不得恒为 PASS。"""

    def _summary_text(self, statuses, verdict):
        results = [
            {"module": f"m{i}", "status": s, "old_ops": 100, "new_ops": 90,
             "delta_pct": -10.0}
            for i, s in enumerate(statuses)
        ]
        with tempfile.TemporaryDirectory() as tmp:
            summary = Path(tmp) / "step_summary.md"
            saved = os.environ.get("GITHUB_STEP_SUMMARY")
            os.environ["GITHUB_STEP_SUMMARY"] = str(summary)
            try:
                ci_perf_check.write_github_summary(results, verdict)
            finally:
                if saved is None:
                    os.environ.pop("GITHUB_STEP_SUMMARY", None)
                else:
                    os.environ["GITHUB_STEP_SUMMARY"] = saved
            return summary.read_text()

    def test_overall_warn_is_printed(self):
        text = self._summary_text(["WARN"], "WARN")
        self.assertIn("**Overall**: WARN", text)
        self.assertIn("⚠️ WARN", text)

    def test_overall_fail_is_printed_not_pass(self):
        text = self._summary_text(["FAIL", "PASS"], "FAIL")
        self.assertIn("**Overall**: FAIL", text)
        self.assertNotIn("**Overall**: PASS", text)

    def test_overall_metric_invalid_is_printed(self):
        text = self._summary_text(["METRIC_INVALID"], "METRIC_INVALID")
        self.assertIn("**Overall**: METRIC_INVALID", text)

    def test_no_summary_env_is_silent(self):
        os.environ.pop("GITHUB_STEP_SUMMARY", None)
        ci_perf_check.write_github_summary([], "FAIL")  # 不抛异常即通过


class DetectStallTest(unittest.TestCase):
    """detect_stall 必须区分冻结与进程失败，且按 name 精确匹配样本。"""

    @staticmethod
    def _result(returncode, stdout="", timed_out=False):
        category = ("timeout" if timed_out
                    else "crash" if (returncode or 0) < 0
                    else "failure" if returncode else "pass")
        return CommandResult(returncode=returncode, stdout=stdout, stderr="",
                             category=category, timed_out=timed_out,
                             duration=1.0)

    def _detect(self, result):
        with mock.patch.object(ci_perf_check, "run_command", return_value=result):
            return ci_perf_check.detect_stall("comutex", 10)

    def test_crash_signal_is_process_failure(self):
        self.assertEqual(self._detect(self._result(-11)), "process_failure")

    def test_nonzero_exit_is_process_failure(self):
        self.assertEqual(self._detect(self._result(1)), "process_failure")

    def test_timeout_is_stall(self):
        self.assertEqual(self._detect(self._result(None, timed_out=True)), "stall")

    def test_insufficient_samples_is_no_data(self):
        stdout = ('FATIGUE_METRIC:{"ts":1.0,"name":"comutex","elapsed_s":2.0,'
                  '"ops_total":100,"errors":0}\n')
        self.assertEqual(self._detect(self._result(0, stdout)), "no_data")

    def test_steady_growth_is_ok(self):
        lines = [
            f'FATIGUE_METRIC:{{"ts":{i}.0,"name":"comutex","elapsed_s":{i + 1}.0,'
            f'"ops_total":{i * 1000},"errors":0}}'
            for i in (1, 2, 3, 4)
        ]
        self.assertEqual(self._detect(self._result(0, "\n".join(lines))), "ok")

    def test_frozen_counter_is_stall(self):
        lines = [
            'FATIGUE_METRIC:{"ts":1.0,"name":"comutex","elapsed_s":2.0,"ops_total":100,"errors":0}',
            'FATIGUE_METRIC:{"ts":2.0,"name":"comutex","elapsed_s":4.0,"ops_total":100,"errors":0}',
            'FATIGUE_METRIC:{"ts":3.0,"name":"comutex","elapsed_s":6.0,"ops_total":100,"errors":0}',
        ]
        self.assertEqual(self._detect(self._result(0, "\n".join(lines))), "stall")

    def test_field_substring_not_matched_as_module(self):
        # 行内 "chan_reads" 等字段含 "chan" 子串：旧实现会误当模块 chan 样本，
        # 修复后按 name 精确匹配，模块 chan 无样本 → no_data。
        lines = [
            'FATIGUE_METRIC:{"ts":1.0,"name":"comutex","elapsed_s":2.0,"ops_total":100,"errors":0,"chan_reads":5}',
            'FATIGUE_METRIC:{"ts":2.0,"name":"comutex","elapsed_s":4.0,"ops_total":200,"errors":0,"chan_reads":6}',
        ]
        with mock.patch.object(ci_perf_check, "run_command",
                               return_value=self._result(0, "\n".join(lines))):
            self.assertEqual(ci_perf_check.detect_stall("chan", 10), "no_data")


def _full_env(agent="perf-a"):
    """构造完整指纹：与 FINGERPRINT_KEYS 一致，任一键缺失都会判不一致。"""
    return {
        "agent": agent, "node": agent, "cpu_model": "Intel X",
        "logical_cores": 8, "mem_total_kb": "16000000kB",
        "compiler": "g++ 13", "cmake_version": "3.28",
        "ninja_version": "1.11", "build_type": "Release",
        "cmake_args": [], "threads": 2,
    }


def _baseline_file(env, ops_map, micro=None):
    """构造统一 Schema 基线 dict（compare_baselines 输入）。"""
    data = {
        "schema_version": 1, "repository": "bbtools-coroutine",
        "commit": "abc123", "base_commit": None, "environment": env,
        "build": {"type": "Release"}, "parameters": {"threads": 2},
        "modules": {
            m: {"ops_total": int(v * 10), "ops_per_sec": float(v),
                "errors": 0, "elapsed_s": 10.0}
            for m, v in ops_map.items()
        },
        "verdict": "PASS",
    }
    if micro is not None:
        data["micro_bench"] = micro
    return data


class CompareBaselinesTest(unittest.TestCase):
    """compare_baselines 聚合逻辑：环境不一致必须 NCB，micro 随 comparable 门控。"""

    @staticmethod
    def _ops(value):
        return {m: float(value) for m in record_baseline.MODULES}

    def _run_compare(self, b1, b2):
        with tempfile.TemporaryDirectory() as tmp:
            p1, p2 = Path(tmp) / "old.json", Path(tmp) / "new.json"
            p1.write_text(json.dumps(b1))
            p2.write_text(json.dumps(b2))
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = record_baseline.compare_baselines(str(p1), str(p2))
            return rc, buf.getvalue()

    def test_env_mismatch_overall_is_ncb(self):
        env1, env2 = _full_env("perf-a"), _full_env("perf-b")
        rc, out = self._run_compare(
            _baseline_file(env1, self._ops(1000)),
            _baseline_file(env2, self._ops(1000)),
        )
        self.assertIn("Overall verdict: NO_COMPARABLE_BASELINE", out)
        self.assertNotIn("Overall verdict: PASS", out)
        self.assertNotIn("Overall verdict: FAIL", out)
        self.assertEqual(rc, 0)

    def test_env_match_all_pass(self):
        env = _full_env()
        rc, out = self._run_compare(
            _baseline_file(env, self._ops(1000)),
            _baseline_file(dict(env), self._ops(1050)),
        )
        self.assertIn("Overall verdict: PASS", out)
        self.assertEqual(rc, 0)

    def test_gated_drop_is_fail(self):
        env = _full_env()
        new = self._ops(1000)
        new["comutex"] = 700.0  # -30%：compare gate_enabled=True → FAIL
        rc, out = self._run_compare(
            _baseline_file(env, self._ops(1000)),
            _baseline_file(dict(env), new),
        )
        self.assertIn("Overall verdict: FAIL", out)
        self.assertEqual(rc, 1)

    def test_micro_bench_gated_by_comparable(self):
        micro = {"yield_roundtrip_avg_ns": 100.0}
        ops = self._ops(1000)
        env1, env2 = _full_env("perf-a"), _full_env("perf-b")
        rc, out = self._run_compare(
            _baseline_file(env1, ops, micro), _baseline_file(env2, ops, micro))
        self.assertNotIn("co_switch", out)  # 环境不一致：micro 不展示
        self.assertEqual(rc, 0)
        env = _full_env()
        _, out_ok = self._run_compare(
            _baseline_file(env, ops, micro), _baseline_file(dict(env), ops, micro))
        self.assertIn("co_switch", out_ok)  # 环境一致：micro 展示


class RecordEnvironmentTest(unittest.TestCase):
    """collect_environment 的 durations 必须用实际时长参数，而非硬编码 60。"""

    def test_durations_use_actual_param(self):
        env = record_baseline.collect_environment(threads=2, duration=12)
        self.assertEqual(env["durations"], [12] * len(record_baseline.MODULES))

    def test_durations_respect_quick_cap(self):
        env = record_baseline.collect_environment(threads=2, duration=120, quick=True)
        self.assertEqual(env["durations"], [60] * len(record_baseline.MODULES))


class RecordMainTailTest(unittest.TestCase):
    """record_baseline main 尾部：compare 自动发现、退出码传播（旧 review 包截断处）。"""

    @staticmethod
    def _ops(value):
        return {m: float(value) for m in record_baseline.MODULES}

    def _write_baseline(self, d, name, env, ops_value):
        (d / name).write_text(json.dumps(_baseline_file(env, self._ops(ops_value))))

    def test_compare_auto_requires_two_baselines(self):
        slug = re.sub(r"[^\w\-]", "_", platform.node())
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp) / slug
            d.mkdir(parents=True)
            (d / "only.json").write_text(json.dumps({"schema_version": 1}))
            with mock.patch.object(record_baseline, "BASELINE_DIR", tmp), \
                    mock.patch.object(sys, "argv",
                                      ["record_baseline.py", "compare"]), \
                    contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit) as ctx:
                    record_baseline.main()
            self.assertEqual(ctx.exception.code, 1)

    def test_compare_auto_picks_two_latest_and_exits_zero(self):
        slug = re.sub(r"[^\w\-]", "_", platform.node())
        env = _full_env()
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp) / slug
            d.mkdir(parents=True)
            self._write_baseline(d, "20260815_000001_abc.json", env, 1000)
            self._write_baseline(d, "20260815_000002_def.json", env, 1050)
            buf = io.StringIO()
            with mock.patch.object(record_baseline, "BASELINE_DIR", tmp), \
                    mock.patch.object(sys, "argv",
                                      ["record_baseline.py", "compare"]), \
                    contextlib.redirect_stdout(buf):
                with self.assertRaises(SystemExit) as ctx:
                    record_baseline.main()
            self.assertEqual(ctx.exception.code, 0)
            self.assertIn("Overall verdict: PASS", buf.getvalue())


if __name__ == "__main__":
    unittest.main()
