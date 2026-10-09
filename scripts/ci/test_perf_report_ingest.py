#!/usr/bin/env python3
"""perf_report_ingest.py 的行为测试（离线、真实子进程执行）。

所有 fixture 都是**测试合成数据**（author id 78525443 为公开拥有者 API id，
其余 repo/sha/env/metrics 均为构造值），不来自任何真实评论；本测试不联网、
不写仓库、不执行被测源码之外的代码。

覆盖：成功(PASS/WARN)、身份冒充忽略但不阻断合法评论、同 head 冲突、缺报告、
source/head 不符、baseline ref/commit/sha 不符、环境不可比、metrics 非法、
verdict 越集 / 声明比实测更轻、measurement_sha256 被篡改、以及消费脚本只依赖
stdlib 且不执行评论内容。另含与 perf_contract 真源的阈值/指纹键 drift guard。
"""

import base64
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
REPO_DIR = HERE.parent.parent
INGEST = HERE / "perf_report_ingest.py"
sys.path.insert(0, str(HERE))

import perf_report_ingest as ingest  # noqa: E402

REPO = "yqm-307/bbtools-coroutine"
OWNER_ID = 78525443  # 公开拥有者 API user id（exact 比较基准）
SOURCE_SHA = "5e201bcecb90fcd18288d1dbdb3a61287c7a3014"
BASELINE_COMMIT = "564e9a1d3e8424f94f5103c0d45042fbb5f2d0c4"
BASELINE_REF = "refs/heads/perf-baseline"
BASELINE_PATH = "tests/baselines/host/perf.json"
MODULES = ("comutex", "corwmutex", "cocond", "chan", "copool", "coroutine")


def full_env(agent="perf-a"):
    return {"agent": agent, "node": agent, "cpu_model": "Intel X",
            "logical_cores": 8, "mem_total_kb": "16000000kB",
            "compiler": "g++ 13", "cmake_version": "3.28",
            "ninja_version": "1.11", "build_type": "Release",
            "cmake_args": [], "threads": 2}


def baseline_bytes(env, ops=1000.0):
    modules = {m: {"ops_total": int(ops * 45), "ops_per_sec": ops,
                   "errors": 0, "elapsed_s": 45.0} for m in MODULES}
    doc = {"schema_version": 1, "repository": REPO, "commit": "c" * 40,
           "base_commit": None, "environment": env, "build": {"type": "Release"},
           "parameters": {"threads": 2}, "modules": modules, "verdict": "PASS"}
    return json.dumps(doc).encode("utf-8")


def report_doc(new_ops, baseline_raw, env, verdict="PASS", *, source_sha=SOURCE_SHA,
               repo=REPO, ref=BASELINE_REF, commit=BASELINE_COMMIT,
               path=BASELINE_PATH, baseline_sha=None, modules=None, declared=None):
    modules = modules or {m: {"ops_total": int(o * 45), "ops_per_sec": o,
                              "errors": 0, "elapsed_s": 45.0, "status": "PASS"}
                          for m, o in new_ops.items()}
    report = {"schema": ingest.MARKER, "schema_version": 1, "repo": repo,
              "source_sha": source_sha,
              "baseline": {"ref": ref, "commit": commit, "path": path,
                           "sha256": baseline_sha or hashlib.sha256(baseline_raw).hexdigest()},
              "environment": env, "parameters": {"threads": 2, "dur": 45, "gate_enabled": True},
              "modules": modules, "verdict": verdict}
    digest = declared or hashlib.sha256(ingest.canonical_bytes(report)).hexdigest()
    report["measurement_sha256"] = digest
    return report


def comment(report, author_id=OWNER_ID, login="yqm-307", marker=True, body_only=None):
    if body_only is not None:
        body = body_only
    else:
        head = "<!-- bbtools-server3-perf/v1 -->\n" if marker else "unrelated\n"
        body = head + "```json\n" + json.dumps(report) + "\n```\n"
    return {"id": 1, "user": {"id": author_id, "login": login}, "body": body}


class IngestCliTest(unittest.TestCase):
    """端到端：真实子进程 + 退出码。"""

    def _run(self, comments, baseline_raw=None, *, expected_author=OWNER_ID,
             source_sha=SOURCE_SHA, expected_commit=BASELINE_COMMIT, repo=REPO):
        with tempfile.TemporaryDirectory() as tmp:
            cpath = Path(tmp) / "comments.json"
            cpath.write_text(json.dumps(comments), encoding="utf-8")
            args = [sys.executable, str(INGEST), "--repo", repo,
                    "--source-sha", source_sha,
                    "--expected-author-id", str(expected_author),
                    "--expected-baseline-ref", BASELINE_REF,
                    "--expected-baseline-commit", expected_commit,
                    "--gate-enabled", "--comments-file", str(cpath)]
            if baseline_raw is not None:
                bpath = Path(tmp) / "baseline.json"
                bpath.write_bytes(baseline_raw)
                args += ["--baseline-file", str(bpath)]
            proc = subprocess.run(args, capture_output=True, text=True)
            return proc.returncode, proc.stdout, proc.stderr

    def test_valid_pass_report_succeeds(self):
        env = full_env()
        raw = baseline_bytes(env)
        report = report_doc({m: 1000.0 for m in MODULES}, raw, env, "PASS")
        rc, out, _ = self._run([comment(report)], raw)
        self.assertEqual(rc, 0, out)
        self.assertIn("PASS", out)

    def test_valid_warn_report_succeeds_without_blocking(self):
        env = full_env()
        raw = baseline_bytes(env)
        report = report_doc({m: 850.0 for m in MODULES}, raw, env, "WARN")  # -15% → WARN
        rc, out, err = self._run([comment(report)], raw)
        self.assertEqual(rc, 0, out + err)

    def test_spoofed_author_ignored_and_real_comment_still_passes(self):
        env = full_env()
        raw = baseline_bytes(env)
        real = report_doc({m: 1000.0 for m in MODULES}, raw, env, "PASS")
        spoof = report_doc({m: 1.0 for m in MODULES}, raw, env, "FAIL",
                           source_sha=SOURCE_SHA)
        rc, out, _ = self._run([comment(spoof, author_id=999),
                                comment(real)], raw)
        self.assertEqual(rc, 0, out)

    def test_lookalike_login_but_wrong_id_is_ignored(self):
        env = full_env()
        raw = baseline_bytes(env)
        fake = report_doc({m: 1000.0 for m in MODULES}, raw, env, "PASS")
        rc, _, err = self._run([comment(fake, author_id=1, login="yqm-307")], raw)
        self.assertEqual(rc, 2)
        self.assertIn("REPORT_MISSING", err)

    def test_missing_report_fails_closed(self):
        env = full_env()
        raw = baseline_bytes(env)
        rc, _, err = self._run([], raw)
        self.assertEqual(rc, 2)
        self.assertIn("REPORT_MISSING", err)

    def test_unrelated_comment_without_marker_is_ignored(self):
        env = full_env()
        raw = baseline_bytes(env)
        rc, _, err = self._run([comment({}, body_only="just chatting\n")], raw)
        self.assertEqual(rc, 2)
        self.assertIn("REPORT_MISSING", err)

    def test_source_sha_mismatch_is_report_missing(self):
        env = full_env()
        raw = baseline_bytes(env)
        report = report_doc({m: 1000.0 for m in MODULES}, raw, env, "PASS",
                            source_sha="a" * 40)
        rc, _, err = self._run([comment(report)], raw)
        self.assertEqual(rc, 2)
        self.assertIn("REPORT_MISSING", err)

    def test_conflicting_reports_for_same_sha_fail(self):
        env = full_env()
        raw = baseline_bytes(env)
        good = report_doc({m: 1000.0 for m in MODULES}, raw, env, "PASS")
        other = report_doc({m: 900.0 for m in MODULES}, raw, env, "WARN")
        rc, _, err = self._run([comment(good), comment(other)], raw)
        self.assertEqual(rc, 2)
        self.assertIn("CONFLICTING_REPORTS", err)

    def test_baseline_ref_mismatch_fails(self):
        env = full_env()
        raw = baseline_bytes(env)
        report = report_doc({m: 1000.0 for m in MODULES}, raw, env, "PASS",
                            ref="refs/heads/other")
        rc, _, err = self._run([comment(report)], raw)
        self.assertEqual(rc, 2)
        self.assertIn("NO_COMPARABLE_BASELINE", err)

    def test_pinned_baseline_commit_mismatch_fails(self):
        env = full_env()
        raw = baseline_bytes(env)
        report = report_doc({m: 1000.0 for m in MODULES}, raw, env, "PASS",
                            commit="d" * 40)
        rc, _, err = self._run([comment(report)], raw)
        self.assertEqual(rc, 2)
        self.assertIn("NO_COMPARABLE_BASELINE", err)

    def test_baseline_bytes_sha_mismatch_fails(self):
        env = full_env()
        raw = baseline_bytes(env)
        report = report_doc({m: 1000.0 for m in MODULES}, raw, env, "PASS")
        tampered = baseline_bytes(env, ops=999.0)  # 与 report.baseline.sha256 不符
        rc, _, err = self._run([comment(report)], tampered)
        self.assertEqual(rc, 2)
        self.assertIn("NO_COMPARABLE_BASELINE", err)

    def test_environment_mismatch_fails(self):
        env = full_env()
        raw = baseline_bytes(env)
        report = report_doc({m: 1000.0 for m in MODULES}, raw, full_env("perf-b"), "PASS")
        rc, _, err = self._run([comment(report)], raw)
        self.assertEqual(rc, 2)
        self.assertIn("NO_COMPARABLE_BASELINE", err)

    def test_declared_lighter_than_derived_fails(self):
        env = full_env()
        raw = baseline_bytes(env)
        # 实测 -30%（gate on → FAIL），报告却声明 PASS → 不可信
        report = report_doc({m: 700.0 for m in MODULES}, raw, env, "PASS")
        rc, _, err = self._run([comment(report)], raw)
        self.assertEqual(rc, 2)
        self.assertIn("METRIC_INVALID", err)

    def test_declared_fail_is_not_success(self):
        env = full_env()
        raw = baseline_bytes(env)
        report = report_doc({m: 700.0 for m in MODULES}, raw, env, "FAIL")
        rc, _, err = self._run([comment(report)], raw)
        self.assertEqual(rc, 2)
        self.assertIn("FAIL", err)

    def test_no_comparable_verdict_is_not_success(self):
        env = full_env()
        raw = baseline_bytes(env)
        report = report_doc({m: 1000.0 for m in MODULES}, raw, env,
                            "NO_COMPARABLE_BASELINE")
        rc, _, err = self._run([comment(report)], raw)
        self.assertEqual(rc, 2)
        self.assertIn("NO_COMPARABLE_BASELINE", err)

    def test_verdict_outside_closed_set_fails(self):
        env = full_env()
        raw = baseline_bytes(env)
        report = report_doc({m: 1000.0 for m in MODULES}, raw, env, "MAYBE")
        rc, _, err = self._run([comment(report)], raw)
        self.assertEqual(rc, 2)
        self.assertIn("METRIC_INVALID", err)

    def test_module_set_not_exactly_six_fails(self):
        env = full_env()
        raw = baseline_bytes(env)
        modules = {m: {"ops_total": 45000, "ops_per_sec": 1000.0, "errors": 0,
                       "elapsed_s": 45.0, "status": "PASS"}
                   for m in ("comutex", "corwmutex")}
        report = report_doc({}, raw, env, "PASS", modules=modules)
        rc, _, err = self._run([comment(report)], raw)
        self.assertEqual(rc, 2)
        self.assertIn("METRIC_INVALID", err)

    def test_nonzero_errors_fails(self):
        env = full_env()
        raw = baseline_bytes(env)
        modules = {m: {"ops_total": 45000, "ops_per_sec": 1000.0, "errors": 0,
                       "elapsed_s": 45.0, "status": "PASS"} for m in MODULES}
        modules["chan"]["errors"] = 3
        report = report_doc({}, raw, env, "PASS", modules=modules)
        rc, _, err = self._run([comment(report)], raw)
        self.assertEqual(rc, 2)
        self.assertIn("METRIC_INVALID", err)

    def test_tampered_measurement_digest_fails(self):
        env = full_env()
        raw = baseline_bytes(env)
        report = report_doc({m: 1000.0 for m in MODULES}, raw, env, "PASS",
                            declared="0" * 64)
        rc, _, err = self._run([comment(report)], raw)
        self.assertEqual(rc, 2)
        self.assertIn("REPORT_MALFORMED", err)

    def test_bad_source_sha_argument_fails(self):
        rc, _, err = self._run([], None, source_sha="nothex")
        self.assertEqual(rc, 2)
        self.assertIn("40-hex", err)

    def test_bad_author_id_argument_fails(self):
        rc, _, err = self._run([], None, expected_author="abc")
        self.assertEqual(rc, 2)
        self.assertIn("positive integer", err)

    def test_infinite_metric_report_fails_closed(self):
        # MF-2 端到端复现：ops_per_sec=Infinity 必须 fail-closed（此前会误判 PASS/exit 0）。
        env = full_env()
        raw = baseline_bytes(env)
        modules = {m: {"ops_total": 45000, "ops_per_sec": 1000.0, "errors": 0,
                       "elapsed_s": 45.0, "status": "PASS"} for m in MODULES}
        modules["chan"]["ops_per_sec"] = float("inf")
        report = report_doc({}, raw, env, "PASS", modules=modules)
        rc, _, err = self._run([comment(report)], raw)
        self.assertEqual(rc, 2)
        self.assertIn("METRIC_INVALID", err)


def cli_args(comments_path, *, baseline_path=None, repo=REPO, source_sha=SOURCE_SHA,
             author=OWNER_ID, commit=BASELINE_COMMIT):
    argv = ["--repo", repo, "--source-sha", source_sha,
            "--expected-author-id", str(author),
            "--expected-baseline-ref", BASELINE_REF,
            "--expected-baseline-commit", commit,
            "--gate-enabled", "--comments-file", str(comments_path)]
    if baseline_path is not None:
        argv += ["--baseline-file", str(baseline_path)]
    return ingest.build_parser().parse_args(argv)


class ContentsApiFetchPathTest(unittest.TestCase):
    """MF-1：真实 Contents API 的换行 base64 必须可解码；非法输入仍必须拒绝。"""

    def _fetch(self, content, encoding="base64"):
        payload = {"encoding": encoding, "content": content}
        with mock.patch.object(ingest, "http_get_json",
                               return_value=(payload, None)):
            return ingest.fetch_baseline_bytes(REPO, BASELINE_PATH, BASELINE_COMMIT, "")

    def test_wrapped_base64_from_contents_api_decodes(self):
        raw = b'{"schema_version": 1}\n' * 40
        wrapped = base64.encodebytes(raw).decode("ascii")  # 每 76 字符插入换行
        self.assertIn("\n", wrapped)
        self.assertEqual(self._fetch(wrapped), raw)

    def test_crlf_and_space_wrapped_base64_decodes(self):
        raw = os.urandom(333)
        flat = base64.b64encode(raw).decode("ascii")
        chunked = "\r\n".join(flat[i:i + 60] for i in range(0, len(flat), 60)) + "\n"
        self.assertEqual(self._fetch(chunked), raw)

    def test_invalid_base64_still_rejected(self):
        with self.assertRaises(ingest.IngestError) as ctx:
            self._fetch("this*is*not*base64")
        self.assertEqual(ctx.exception.status, "NO_COMPARABLE_BASELINE")


class MetricFinitenessTest(unittest.TestCase):
    """MF-2：ops_total/ops_per_sec/elapsed_s 必须是 bool 之外的非负有限数。"""

    def _report_with(self, field, value):
        env = full_env()
        raw = baseline_bytes(env)
        modules = {m: {"ops_total": 45000, "ops_per_sec": 1000.0, "errors": 0,
                       "elapsed_s": 45.0, "status": "PASS"} for m in MODULES}
        modules["chan"][field] = value
        return report_doc({}, raw, env, "PASS", modules=modules)

    def test_nonfinite_or_bool_metrics_rejected(self):
        for field in ("ops_total", "ops_per_sec", "elapsed_s"):
            for value in (float("inf"), float("nan"), float("-inf"), True):
                report = self._report_with(field, value)
                with self.assertRaises(ingest.IngestError,
                                       msg=f"{field}={value!r}") as ctx:
                    ingest.validate_report(report, REPO, SOURCE_SHA,
                                           BASELINE_REF, BASELINE_COMMIT)
                self.assertEqual(ctx.exception.status, "METRIC_INVALID",
                                 f"{field}={value!r}")

    def test_finite_metrics_still_accepted(self):
        report = self._report_with("ops_per_sec", 1000.0)
        ingest.validate_report(report, REPO, SOURCE_SHA, BASELINE_REF, BASELINE_COMMIT)


class ValidateBeforeFetchTest(unittest.TestCase):
    """SF-1：结构校验必须早于任何 baseline 网络读取；缺 baseline 键须干净失败。"""

    def test_unapproved_baseline_path_never_triggers_fetch(self):
        env = full_env()
        raw = baseline_bytes(env)
        report = report_doc({m: 1000.0 for m in MODULES}, raw, env, "PASS",
                            path="../../etc/hostname")
        with tempfile.TemporaryDirectory() as tmp:
            cpath = Path(tmp) / "comments.json"
            cpath.write_text(json.dumps([comment(report)]), encoding="utf-8")
            with mock.patch.object(ingest, "fetch_baseline_bytes") as fetch:
                with self.assertRaises(ingest.IngestError) as ctx:
                    ingest._run_once(cli_args(cpath))
                fetch.assert_not_called()
        self.assertEqual(ctx.exception.status, "NO_COMPARABLE_BASELINE")

    def test_missing_baseline_key_is_clean_failure_not_keyerror(self):
        env = full_env()
        raw = baseline_bytes(env)
        report = report_doc({m: 1000.0 for m in MODULES}, raw, env, "PASS")
        del report["baseline"]
        report["measurement_sha256"] = hashlib.sha256(ingest.canonical_bytes(
            {k: v for k, v in report.items()
             if k != "measurement_sha256"})).hexdigest()
        with tempfile.TemporaryDirectory() as tmp:
            cpath = Path(tmp) / "comments.json"
            cpath.write_text(json.dumps([comment(report)]), encoding="utf-8")
            with mock.patch.object(ingest, "fetch_baseline_bytes") as fetch:
                with self.assertRaises(ingest.IngestError) as ctx:
                    ingest._run_once(cli_args(cpath))
                fetch.assert_not_called()
        self.assertEqual(ctx.exception.status, "NO_COMPARABLE_BASELINE")


class CommentsBoundedFetchTest(unittest.TestCase):
    """SF-2/SF-3：分页耗尽必须失败；单评论正文必须有界。"""

    def test_pagination_exhaustion_is_reported_malformed(self):
        def _fake(url, token, *, bounded_bytes, page=None):
            return [], (page or 0) + 1  # 永远还有下一页，直到帧尽 MAX_PAGES

        with mock.patch.object(ingest, "http_get_json", side_effect=_fake):
            with self.assertRaises(ingest.IngestError) as ctx:
                ingest.fetch_comments(REPO, SOURCE_SHA, "")
        self.assertEqual(ctx.exception.status, "REPORT_MALFORMED")

    def test_oversized_single_comment_body_is_reported_malformed(self):
        batch = [{"id": 1, "user": {"id": OWNER_ID},
                  "body": "a" * (ingest.MAX_COMMENT_BYTES + 1)}]
        with mock.patch.object(ingest, "http_get_json", return_value=(batch, None)):
            with self.assertRaises(ingest.IngestError) as ctx:
                ingest.fetch_comments(REPO, SOURCE_SHA, "")
        self.assertEqual(ctx.exception.status, "REPORT_MALFORMED")


class ContractDriftGuardTest(unittest.TestCase):
    """内联常量必须与 perf_contract 真源一致，防阈值/指纹键静默漂移。"""

    def test_thresholds_and_keys_match_perf_contract(self):
        import perf_contract
        self.assertEqual(ingest.THRESHOLD_WARN, perf_contract.THRESHOLD_WARN)
        self.assertEqual(ingest.THRESHOLD_FAIL, perf_contract.THRESHOLD_FAIL)
        self.assertEqual(ingest.THRESHOLD_COCOND, perf_contract.THRESHOLD_COCOND)
        self.assertEqual(tuple(ingest.FINGERPRINT_KEYS), tuple(perf_contract.FINGERPRINT_KEYS))
        self.assertEqual(ingest.VALID_VERDICTS, set(perf_contract.VALID_VERDICTS))
        self.assertEqual(set(ingest.MODULES), {"comutex", "corwmutex", "cocond",
                                               "chan", "copool", "coroutine"})

    def test_derived_status_matches_perf_contract(self):
        import perf_contract
        for module in ("comutex", "cocond"):
            for new_ops in (1100, 901, 900, 850, 800, 700, 600):
                mine, _ = ingest.derive_module_status(1000, new_ops, module, True)
                theirs = perf_contract.compare_module(
                    1000, new_ops, module, gate_enabled=True).status
                self.assertEqual(mine, theirs, f"{module}@{new_ops}")

    def test_metric_value_finiteness_matches_perf_contract(self):
        # MF-2 parity：消费端 metric_value_ok 必须与真源 normalize_module_metrics 同判。
        import perf_contract
        for value in (1000.0, 0, 0.0, float("inf"), float("nan"),
                      float("-inf"), True, False, "1", -5):
            raw = {"ops_total": value, "errors": 0, "elapsed_s": 45.0}
            theirs = perf_contract.normalize_module_metrics(raw) is not None
            self.assertEqual(ingest.metric_value_ok(value), theirs,
                             f"divergence for ops_total={value!r}")


class StdlibOnlyGuardTest(unittest.TestCase):
    """消费脚本不得执行评论内容、不得引入 subprocess/eval/exec/网络外副作用。"""

    def test_source_has_no_code_execution_primitives(self):
        src = INGEST.read_text(encoding="utf-8")
        for needle in ("subprocess", "os.system", "eval(", "exec(",
                       "os.popen", "shell=True", "__import__("):
            self.assertNotIn(needle, src, f"forbidden primitive: {needle}")

    def test_comment_body_is_never_executed(self):
        # 恶意正文里的“代码”只会被 json.loads；非 JSON 段必须被忽略而非执行。
        body = "<!-- bbtools-server3-perf/v1 -->\n```json\nraise SystemExit(0)\n```\n"
        self.assertIsNone(ingest.extract_report(body))

    def test_only_stdlib_imports(self):
        src = INGEST.read_text(encoding="utf-8")
        for mod in ("import ci_common", "import perf_contract", "from ci_",
                    "import record_baseline", "import ci_perf_check"):
            self.assertNotIn(mod, src)


if __name__ == "__main__":
    unittest.main()
