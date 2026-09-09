#!/usr/bin/env python3
"""Offline release/CI regression checks; fixtures are not acceptance evidence."""
import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock
import urllib.error

import release_gate as gate

ROOT = Path(__file__).resolve().parent.parent
SHA = 'a' * 40


class ReleaseGateTest(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch.dict(os.environ, {'GITHUB_REPOSITORY': 'owner/repo', 'GITHUB_TOKEN': 'offline-fixture', 'GITHUB_RUN_ID': '123'})
        patcher.start()
        self.addCleanup(patcher.stop)
        silence = contextlib.redirect_stderr(io.StringIO())
        silence.__enter__()
        self.addCleanup(silence.__exit__, None, None, None)

    def test_input_grammar_and_no_network_for_bad_inputs(self):
        for version in ('v3.0.0', 'v3.0.0-rc0', 'v3.0.0-rc1'):
            gate.parse_version(version)
        for version in ('3.0.0', 'v3.0', 'v03.0.0', 'v3.0.0-rc01', 'v٣.0.0', 'v3.0.0\n', 'v3.0.0;id'):
            with self.subTest(version=version), self.assertRaises(SystemExit):
                gate.parse_version(version)
        for args in [('rc', 'v3.0.0', SHA, None), ('rc', 'v3.0.0-rc1', 'main', None),
                     ('stable', 'v3.0.0', SHA, 'v3.1.0-rc1')]:
            with mock.patch.object(gate, 'api') as api, self.assertRaises(SystemExit):
                gate.publish(*args)
            api.assert_not_called()

    def test_api_only_treats_404_as_absent(self):
        for code in (403, 404, 500):
            error = urllib.error.HTTPError('https://example.invalid', code, '', {}, None)
            with mock.patch.object(gate.urllib.request, 'urlopen', side_effect=error):
                if code == 404:
                    self.assertEqual(gate.api('test', allow_404=True), {})
                else:
                    with self.assertRaises(SystemExit):
                        gate.api('test', allow_404=True)

    def test_existing_tag_or_release_blocks_write(self):
        for responses in ([{'object': {'sha': SHA}}], [{}, {'tag_name': 'v3.0.0'}]):
            with mock.patch.object(gate, 'api', side_effect=responses), self.assertRaises(SystemExit):
                gate.ensure_absent('v3.0.0')

    def test_main_ci_requires_successful_exact_sha_and_all_jobs(self):
        run = {'id': 1, 'head_sha': SHA, 'head_branch': 'main', 'event': 'push', 'conclusion': 'success'}
        names = ['编译 & 单元测试', '真实客户端验收', '性能回归检查', '1h 并行疲劳压测']
        for conclusion in ('success', 'skipped', 'failure'):
            jobs = [{'name': name, 'conclusion': 'success'} for name in names]
            jobs[-1]['conclusion'] = conclusion
            with mock.patch.object(gate, 'api', side_effect=[{'workflow_runs': [run]}, {'jobs': jobs, 'total_count': len(jobs)}]):
                if conclusion == 'success':
                    gate.validate_main_ci(SHA)
                else:
                    with self.assertRaises(SystemExit):
                        gate.validate_main_ci(SHA)
        with mock.patch.object(gate, 'api', return_value={'workflow_runs': [{**run, 'head_sha': 'b' * 40}]}), self.assertRaises(SystemExit):
            gate.validate_main_ci(SHA)

    def test_rc_main_and_stable_tag_boundaries(self):
        with mock.patch.object(gate, 'ensure_absent'), mock.patch.object(gate, 'validate_main_ci'), mock.patch.object(gate, 'git', return_value=SHA):
            with mock.patch.object(gate, 'api', return_value={'object': {'sha': 'b' * 40}}), self.assertRaises(SystemExit):
                gate.validate_candidate('rc', 'v3.0.0-rc1', SHA, None)
            for draft, tag_sha in [(False, SHA), (True, SHA), (False, 'b' * 40)]:
                responses = [{'object': {'sha': SHA}}, {'object': {'sha': tag_sha, 'type': 'commit'}},
                             {'draft': draft, 'prerelease': True, 'tag_name': 'v3.0.0-rc1'}]
                with mock.patch.object(gate, 'api', side_effect=responses):
                    if not draft and tag_sha == SHA:
                        gate.validate_candidate('stable', 'v3.0.0', SHA, 'v3.0.0-rc1')
                    else:
                        with self.assertRaises(SystemExit):
                            gate.validate_candidate('stable', 'v3.0.0', SHA, 'v3.0.0-rc1')

    def test_publish_readback_and_no_retry_after_uncertain_write(self):
        for tagged_sha in (SHA, 'b' * 40):
            responses = [{}, {'tag_name': 'v3.0.0-rc1', 'prerelease': True, 'draft': False, 'body': SHA},
                         {'object': {'type': 'commit', 'sha': tagged_sha}}]
            with mock.patch.object(gate, 'validate_candidate'), mock.patch.object(gate, 'api', side_effect=responses) as api:
                if tagged_sha == SHA:
                    gate.publish('rc', 'v3.0.0-rc1', SHA, None)
                else:
                    with self.assertRaises(SystemExit):
                        gate.publish('rc', 'v3.0.0-rc1', SHA, None)
                self.assertEqual(sum(c.kwargs.get('method') == 'POST' for c in api.call_args_list), 1)
        with mock.patch.object(gate, 'validate_candidate'), mock.patch.object(gate, 'api', side_effect=SystemExit(2)) as api:
            with self.assertRaises(SystemExit):
                gate.publish('rc', 'v3.0.0-rc1', SHA, None)
            self.assertEqual(api.call_count, 1)

    def test_parallel_stress_exit_and_summary_agree(self):
        # Stand-in child process deliberately emits invalid data to test the real shell harness.
        fixture = '''#!/usr/bin/env python3
import json, os, sys
mode = os.environ['FIXTURE_MODE']
module = next(a.split('=', 1)[1] for a in sys.argv if a.startswith('--module='))
if mode != 'missing':
    data = dict(name=module, ops_total=10, errors=0, elapsed_s=1)
    if mode == 'errors': data['errors'] = 1
    if mode == 'zero': data['ops_total'] = 0
    if mode == 'partial': data['elapsed_s'] = 0.1
    if mode == 'nan': data['ops_total'] = float('nan')
    print('FATIGUE_METRIC:' + json.dumps(data))
sys.exit(1 if mode == 'crash' else 0)
'''
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'scripts').mkdir()
            shutil.copytree(ROOT / 'scripts' / 'ci', root / 'scripts' / 'ci')
            binary = root / 'build/bin/benchmark_test/unified_stress'
            binary.parent.mkdir(parents=True)
            binary.write_text(fixture)
            binary.chmod(0o755)
            for mode in ('pass', 'errors', 'zero', 'partial', 'missing', 'nan', 'crash'):
                outdir = root / mode
                result = subprocess.run(['bash', str(ROOT / 'scripts/run_parallel_stress.sh'), '1', '1'],
                                        cwd=root, env={**os.environ, 'FIXTURE_MODE': mode, 'OUTDIR': str(outdir)},
                                        text=True, capture_output=True, timeout=15)
                summary = (outdir / 'summary.txt').read_text()
                self.assertEqual(result.returncode == 0, mode == 'pass', (mode, result.stderr))
                self.assertEqual(summary.splitlines()[-1].split()[-1], 'PASS' if mode == 'pass' else 'FAIL')

    def test_workflow_performance_policy(self):
        # Execute the actual workflow guard with synthetic reports, not a copied predicate.
        import re
        import sys
        import textwrap
        for workflow, report_path, strict in (
            ('unit_test.yml', 'tests/ci-reports/main-performance.json', False),
            ('release.yml', 'source/tests/ci-reports/release-performance.json', True),
        ):
            text = (ROOT / '.github/workflows' / workflow).read_text()
            blocks = re.findall(r"(?ms)^          python3 - <<'PY'\n(.*?)^          PY$", text)
            self.assertEqual(len(blocks), 1)
            guard = textwrap.dedent(blocks[0])
            for verdict in ('PASS', 'WARN', 'NO_COMPARABLE_BASELINE', 'FAIL', 'METRIC_INVALID'):
                with self.subTest(workflow=workflow, verdict=verdict), tempfile.TemporaryDirectory() as tmp:
                    path = Path(tmp) / report_path
                    path.parent.mkdir(parents=True)
                    path.write_text(json.dumps({'commit': SHA, 'verdict': verdict, 'modules': {
                        name: {'status': verdict, 'errors': 0} for name in
                        ('comutex', 'corwmutex', 'cocond', 'chan', 'copool', 'coroutine')
                    }}))
                    result = subprocess.run([sys.executable, '-c', guard], cwd=tmp,
                                            env={**os.environ, 'SOURCE_SHA': SHA},
                                            text=True, capture_output=True, timeout=10)
                    expected = verdict in {'PASS', 'WARN'} or (not strict and verdict == 'NO_COMPARABLE_BASELINE')
                    self.assertEqual(result.returncode == 0, expected, result.stderr)
        release = (ROOT / '.github/workflows/release.yml').read_text()
        self.assertIn('ctest --test-dir source/build --timeout 60 --output-on-failure', release)
        self.assertRegex(release, r'name: "全量 CTest"\n        timeout-minutes: 15')
        self.assertRegex(release, r'name: "真实客户端验收（Redis 缺失必须失败）"\n        timeout-minutes: 10')

    def test_main_only_integration_jobs(self):
        import re
        text = (ROOT / '.github/workflows/unit_test.yml').read_text()
        for job in ('real-client-acceptance', 'perf-regression', 'stress-test'):
            block = re.search(r'(?ms)^  ' + job + r':\n(.*?)(?=^  [a-z][a-z-]*:|\Z)', text).group(1)
            self.assertIn("if: github.event_name == 'push' && github.ref == 'refs/heads/main'", block)
        self.assertIn('needs: [build-and-test, real-client-acceptance, perf-regression]', text)

    def test_latency_warning_cannot_hide_throughput_failure(self):
        import ci_perf_check as perf
        with tempfile.TemporaryDirectory() as tmp, contextlib.ExitStack() as stack:
            output = str(Path(tmp) / 'report.json')
            stack.enter_context(mock.patch.object(perf.sys, 'argv', ['perf', '--module=comutex', '--dur=1', '--gate-enabled', '--output', output]))
            stack.enter_context(mock.patch.object(perf.os.path, 'exists', return_value=True))
            stack.enter_context(mock.patch.object(perf, 'load_environment', return_value={}))
            stack.enter_context(mock.patch.object(perf.perf_contract, 'compare_environment', return_value=perf.perf_contract.CompareResult(status='PASS')))
            stack.enter_context(mock.patch.object(perf, 'load_baseline', return_value=({'modules': {'comutex': {'ops_per_sec': 100, 'lock_avg_us': 1}}}, 'fixture')))
            stack.enter_context(mock.patch.object(perf, 'run_benchmark', return_value={'ops_total': 50, 'ops_per_sec': 50, 'errors': 0, 'elapsed_s': 1, 'lock_avg_us': 2}))
            stack.enter_context(mock.patch.object(perf, 'get_git_commit', return_value=SHA))
            stack.enter_context(mock.patch.object(perf, 'write_github_summary'))
            with self.assertRaises(SystemExit) as result, contextlib.redirect_stdout(io.StringIO()):
                perf.main()
            self.assertEqual(result.exception.code, 2)
            self.assertEqual(json.loads(Path(output).read_text())['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
