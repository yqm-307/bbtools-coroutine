#!/usr/bin/env python3
"""CI 单次性能豁免（BBT_CI_PERF_WAIVER）回归：真实执行 heredoc 与 CLI。

豁免契约：`PR_NUMBER BASE_SHA HEAD_SHA REASON`（前三字段单 ASCII 空格，理由单行
非空可打印）；仅同仓 pull_request、classification=code、perf=true 时生效。无效/
未配置/过期豁免是**正常拒绝**（rc0、无批准输出），perf 照常跑，不判红。生效时
perf_waiver.py 写 `perf_waived=true`；result job 据此放行明确的 perf-regression
skipped，绝不放行 failure/cancelled/缺失或其他 job 失败，非 PR 事件不可借用。

覆盖：审批+明确 skipped 端到端放行（真实执行 ci.yml `<<'PY'` heredoc → result.py）；
failure/cancelled/缺失仍红；build-test/changes 失败不遮盖；push 借用批准仍红；
自然跳过不变；豁免步骤 if/env/run 接线；CLI 合法批准与全部拒绝形态。

运行：PYTHONDONTWRITEBYTECODE=1 python3 -m unittest scripts/ci/test_perf_waiver.py
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_formal_ci import ENV, CI_YML, RESULT, body, raw, run

HERE = os.path.dirname(os.path.abspath(__file__))
PERF_WAIVER = os.path.join(HERE, "perf_waiver.py")
PR, BASE, HEAD = "451", "a" * 5 + "1" * 35, "b" * 5 + "2" * 35  # 含 a-f 的完整 lowercase SHA
REASON = "flaky scheduler timing, owner approved"
VALID = f"{PR} {BASE} {HEAD} {REASON}"


def _extract_heredoc_py() -> str:
    """抽出 result job `python3 - "$payload" <<'PY'` … `PY` 的完整正文（dedent，不改写）。"""
    text = raw(CI_YML)
    start = text.index("<<'PY'\n") + len("<<'PY'\n")
    end = text.index("\n          PY\n", start)
    return textwrap.dedent(text[start:end])


def _run_result_pipeline(results, classification, perf, event_name, perf_waived=""):
    """真实执行：heredoc 正文（子进程，真实 env）→ payload 文件 → result.py CLI。"""
    with tempfile.TemporaryDirectory() as td:
        payload = os.path.join(td, "payload.json")
        env = dict(ENV, RESULTS_JSON=json.dumps(results), CLASSIFICATION=classification,
                   PERF=perf, EVENT_NAME=event_name, PERF_WAIVED=perf_waived)
        proc = run([sys.executable, "-", payload], input=_extract_heredoc_py(), env=env)
        assert proc.returncode == 0, proc.stderr  # heredoc 自身不得失败
        return subprocess.run([sys.executable, RESULT, "--file", payload],
                              capture_output=True, text=True, check=False, env=ENV)


def _cli(waiver, pr=PR, base=BASE, head=HEAD):
    """真实执行 perf_waiver.py（-I）；返回 (rc, stdout, GITHUB_OUTPUT 内容)。"""
    env = dict(ENV, PR_NUMBER=pr, PR_BASE_SHA=base, PR_HEAD_SHA=head)
    env.pop("WAIVER_APPROVAL", None)
    if waiver is not None:
        env["WAIVER_APPROVAL"] = waiver
    with tempfile.TemporaryDirectory() as td:
        out = os.path.join(td, "out")
        env["GITHUB_OUTPUT"] = out
        proc = run([sys.executable, "-I", PERF_WAIVER], env=env)
        output = Path(out).read_text(encoding="utf-8") if os.path.isfile(out) else ""
    return proc.returncode, proc.stdout, output


class ResultJobWaiverEndToEndTests(unittest.TestCase):
    """result job heredoc 真实执行：审批+明确 skipped 是唯一新增放行通道。"""

    def _results(self, build="success", perf="skipped", changes="success"):
        return {"changes": changes, "docs-check": "success",
                "build-test": build, "perf-regression": perf}

    def _assert_verdict(self, results, rc, verdict, *, classification="code",
                        perf="true", event_name="pull_request", perf_waived=""):
        proc = _run_result_pipeline(results, classification, perf, event_name, perf_waived)
        self.assertEqual(proc.returncode, rc, proc.stdout + proc.stderr)
        self.assertEqual(json.loads(proc.stdout)["verdict"], verdict)

    def test_approved_explicit_skip_is_success(self):
        self._assert_verdict(self._results(), 0, "success", perf_waived="true")

    def test_waiver_never_passes_failed_cancelled_or_missing_perf(self):
        for perf in ("failure", "cancelled", None):
            results = self._results()
            if perf is None:
                del results["perf-regression"]  # 缺失（job 不可达/无结果）
            else:
                results["perf-regression"] = perf
            with self.subTest(perf=perf):
                self._assert_verdict(results, 1, "failure", perf_waived="true")

    def test_waiver_does_not_hide_build_or_changes_failure(self):
        self._assert_verdict(self._results(build="failure"), 1, "failure", perf_waived="true")
        self._assert_verdict(self._results(changes="failure"), 1, "failure", perf_waived="true")

    def test_push_cannot_borrow_approval(self):
        self._assert_verdict(self._results(), 1, "failure",
                             event_name="push", perf_waived="true")

    def test_no_approval_explicit_skip_still_fails(self):
        for waived in ("", "false"):
            with self.subTest(perf_waived=waived):
                self._assert_verdict(self._results(), 1, "failure", perf_waived=waived)

    def test_natural_skips_unchanged(self):
        # docs-only：两个重型都预期跳过；perf=false：仅 perf 预期跳过；dispatch：perf 预期跳过。
        self._assert_verdict({"changes": "success", "docs-check": "success",
                              "build-test": "skipped", "perf-regression": "skipped"},
                             0, "success", classification="docs-only", perf="false")
        self._assert_verdict(self._results(perf="skipped"), 0, "success", perf="false")
        self._assert_verdict(self._results(perf="skipped"), 0, "success",
                             event_name="workflow_dispatch", classification="unknown")


class WaiverStepWiringTests(unittest.TestCase):
    """changes job 豁免步骤接线（单行 run，不走 _block_containing）。"""

    def test_step_if_env_and_run(self):
        stripped = body(CI_YML)
        step = stripped.split("id: perf-waiver", 1)[1].split("- name:", 1)[0]
        self.assertIn("github.event_name == 'pull_request'", step)
        self.assertIn("github.event.pull_request.head.repo.full_name == github.repository", step)
        self.assertIn("steps.diff.outputs.classification == 'code'", step)
        self.assertIn("steps.diff.outputs.perf == 'true'", step)
        self.assertIn("vars.BBT_CI_PERF_WAIVER != ''", step)
        for key in ("WAIVER_APPROVAL: ${{ vars.BBT_CI_PERF_WAIVER }}", "PR_NUMBER:",
                    "PR_BASE_SHA:", "PR_HEAD_SHA:"):
            self.assertIn(key, step)
        self.assertIn("run: python3 -I scripts/ci/perf_waiver.py", step)
        # 输出链：changes outputs.perf_waived → result job env PERF_WAIVED → heredoc 消费。
        self.assertIn("perf_waived: ${{ steps.perf-waiver.outputs.perf_waived }}", stripped)
        self.assertIn("PERF_WAIVED: ${{ needs.changes.outputs.perf_waived }}", stripped)
        self.assertIn('os.environ.get("PERF_WAIVED") == "true"', _extract_heredoc_py())


class PerfWaiverCliTests(unittest.TestCase):
    """CLI：合法豁免批准；其余形态一律 rc0 + 无批准输出（正常拒绝）。"""

    def _assert_reject(self, waiver, **kwargs):
        rc, _, output = _cli(waiver, **kwargs)
        self.assertEqual(rc, 0)
        self.assertNotIn("perf_waived=true", output)

    def test_valid_waiver_approves_and_logs_skipped_prefix(self):
        rc, stdout, output = _cli(VALID)
        self.assertEqual(rc, 0)
        self.assertIn("perf_waived=true", output)
        self.assertIn("SKIPPED", stdout)
        for needle in (PR, BASE, HEAD, REASON):
            self.assertIn(needle, stdout)

    def test_unset_or_invalid_or_stale_is_normal_rejection(self):
        self._assert_reject(None)                                    # 未配置
        self._assert_reject("")                                      # 空
        self._assert_reject(VALID.replace(" ", "  ", 1))             # 双空格
        self._assert_reject(VALID.replace(BASE, BASE.upper()))       # 大写 SHA
        self._assert_reject(f"{PR} {BASE} {HEAD}")                   # 空理由
        self._assert_reject(f"{PR} {'0' * 40} {HEAD} {REASON}")      # 零 base
        self._assert_reject(f"{PR} {BASE} {'0' * 40} {REASON}")      # 零 head
        self._assert_reject(VALID, pr="452")                         # stale PR
        self._assert_reject(VALID, base="c" * 5 + "3" * 35)          # stale base
        self._assert_reject(VALID, head="d" * 5 + "4" * 35)          # stale head


if __name__ == "__main__":
    unittest.main()
