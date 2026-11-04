#!/usr/bin/env python3
"""run_test_profile.sh 离线契约测试（仅标准库，不跑真实 C++ 构建/ctest）。

策略：在临时目录造一个假的 `ctest` 可执行桩（按 argv 分支处理
`-N --show-only=json-v1` 与实跑+`--output-junit`），把桩目录置于 PATH 最前，
真实执行仓内 run_test_profile.sh，断言：
- profile→ctest 选集参数映射（smoke→-L smoke；focused 必须带 --regex/--label-expr；
  full→默认 -LE live|perf，--include-labels 放回）；
- 选中集为空 / --require-count / --require-file 的 fail-closed 拒绝；
- JUnit 校验：注册集=实跑集、skipped/failure/error 一律判失败；
- 未知参数/未知 profile/缺 build-dir 退出码 2。

运行（三仓相同）：
  PYTHONDONTWRITEBYTECODE=1 python3 -m unittest -v scripts/ci/test_test_profile.py
"""
from __future__ import annotations

import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "run_test_profile.sh")

ENV = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")

# 假 ctest：-N --show-only=json-v1 时按 -L/-LE/-R 过滤固定注册表；
# 实跑时把同样过滤后的集合写成 JUnit。桩把收到的 argv 记到 CTEST_ARGV_LOG
# 供断言选集参数。可通过 CTEST_FAKE_SKIP / CTEST_FAKE_NOTRUN 注入 skipped
# 用例与"实跑缺项"，验证契约判失败而非放行。
FAKE_CTEST = r"""#!/usr/bin/env python3
import json, os, re, sys

TESTS = [
    {"name": "a.smoke", "labels": ["smoke", "unit"]},
    {"name": "b.smoke", "labels": ["smoke"]},
    {"name": "c.unit", "labels": ["unit"]},
    {"name": "d.integration", "labels": ["integration"]},
    {"name": "e.live", "labels": ["live"]},
    {"name": "f.perf", "labels": ["perf"]},
]

argv = sys.argv[1:]
log = os.environ.get("CTEST_ARGV_LOG")
if log:
    with open(log, "a") as f:
        f.write(json.dumps(argv) + "\n")

def opt(flag):
    return argv[argv.index(flag) + 1] if flag in argv else None

def selected():
    out = TESTS
    le = opt("-L")
    if le:
        pat = re.compile(le)
        out = [t for t in out if any(pat.search(l) for l in t["labels"])]
    le_ex = opt("-LE")
    if le_ex:
        pat = re.compile(le_ex)
        out = [t for t in out if not any(pat.search(l) for l in t["labels"])]
    rgx = opt("-R")
    if rgx:
        pat = re.compile(rgx)
        out = [t for t in out if pat.search(t["name"])]
    return out

if "-N" in argv or "--show-only=json-v1" in argv:
    tests = [{"name": t["name"], "properties": [
        {"name": "LABELS", "value": t["labels"]}]} for t in selected()]
    print(json.dumps({"tests": tests}))
    sys.exit(0)

junit = opt("--output-junit")
tests = selected()
# 注入：NOTRUN 从实跑集删一个（注册≠实跑）；SKIP 给第一个加 skipped。
notrun = os.environ.get("CTEST_FAKE_NOTRUN")
skip = os.environ.get("CTEST_FAKE_SKIP")
ran = [t for t in tests if t["name"] != notrun]
if junit:
    cases = []
    for t in ran:
        inner = "<skipped/>" if skip and t is ran[0] else ""
        cases.append(f'<testcase name="{t["name"]}">{inner}</testcase>')
    xml = ('<?xml version="1.0"?><testsuite tests="%d">%s</testsuite>'
           % (len(ran), "".join(cases)))
    with open(junit, "w") as f:
        f.write(xml)
print("fake ctest ran %d tests" % len(ran))
sys.exit(0)
"""


def make_env(tmpdir: str) -> tuple[str, str]:
    """造桩环境：返回 (build_dir, argv_log_path)。PATH 前置桩目录。"""
    bin_dir = os.path.join(tmpdir, "bin")
    build_dir = os.path.join(tmpdir, "build")
    os.makedirs(bin_dir)
    os.makedirs(build_dir)
    ctest = os.path.join(bin_dir, "ctest")
    with open(ctest, "w") as f:
        f.write(FAKE_CTEST)
    os.chmod(ctest, os.stat(ctest).st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    argv_log = os.path.join(tmpdir, "argv.log")
    return build_dir, argv_log


def run_profile(args, tmpdir, extra_env=None):
    env = dict(ENV)
    env["PATH"] = os.path.join(tmpdir, "bin") + os.pathsep + env.get("PATH", "")
    env["CTEST_ARGV_LOG"] = os.path.join(tmpdir, "argv.log")
    if extra_env:
        env.update(extra_env)
    return subprocess.run(["bash", SCRIPT] + args, env=env,
                          capture_output=True, text=True)


def read_argv_log(path):
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return [json.loads(line) for line in f]


class ArgContractTests(unittest.TestCase):
    def test_unknown_arg_exit2(self):
        with tempfile.TemporaryDirectory() as td:
            build, _ = make_env(td)
            p = run_profile(["--build-dir", build, "--profile", "smoke", "--bogus"], td)
            self.assertEqual(p.returncode, 2, p.stderr)
            self.assertIn("FATAL", p.stderr)

    def test_unknown_profile_exit2(self):
        with tempfile.TemporaryDirectory() as td:
            build, _ = make_env(td)
            p = run_profile(["--build-dir", build, "--profile", "nightly"], td)
            self.assertEqual(p.returncode, 2, p.stderr)

    def test_missing_build_dir_exit2(self):
        with tempfile.TemporaryDirectory() as td:
            make_env(td)
            p = run_profile(["--build-dir", os.path.join(td, "nope"),
                             "--profile", "smoke"], td)
            self.assertEqual(p.returncode, 2, p.stderr)

    def test_focused_without_selector_exit2(self):
        with tempfile.TemporaryDirectory() as td:
            build, _ = make_env(td)
            p = run_profile(["--build-dir", build, "--profile", "focused"], td)
            self.assertEqual(p.returncode, 2, p.stderr)
            self.assertIn("focused", p.stderr)


class SelectionContractTests(unittest.TestCase):
    def _run_n(self, args, td):
        build, log = make_env(td)
        p = run_profile(["--build-dir", build] + args, td)
        calls = read_argv_log(log)
        self.assertTrue(calls, "ctest 未被调用")
        n_call = calls[0]  # 第一次调用是 -N 选集
        return p, n_call

    def test_smoke_selects_label(self):
        with tempfile.TemporaryDirectory() as td:
            p, n_call = self._run_n(["--profile", "smoke"], td)
            self.assertEqual(p.returncode, 0, p.stderr + p.stdout)
            self.assertIn("-L", n_call)
            self.assertEqual(n_call[n_call.index("-L") + 1], "smoke")
            self.assertIn("2 passed", p.stdout)

    def test_full_excludes_live_perf_by_default(self):
        with tempfile.TemporaryDirectory() as td:
            p, n_call = self._run_n(["--profile", "full"], td)
            self.assertEqual(p.returncode, 0, p.stderr + p.stdout)
            self.assertIn("-LE", n_call)
            self.assertEqual(n_call[n_call.index("-LE") + 1], "live|perf")
            # 6 注册 - live - perf = 4 实跑
            self.assertIn("4 passed", p.stdout)

    def test_full_include_labels_restores(self):
        with tempfile.TemporaryDirectory() as td:
            p, n_call = self._run_n(
                ["--profile", "full", "--include-labels", "live"], td)
            self.assertEqual(p.returncode, 0, p.stderr + p.stdout)
            self.assertEqual(n_call[n_call.index("-LE") + 1], "perf")
            self.assertIn("5 passed", p.stdout)

    def test_focused_regex_selects(self):
        with tempfile.TemporaryDirectory() as td:
            p, n_call = self._run_n(
                ["--profile", "focused", "--regex", "^a\\."], td)
            self.assertEqual(p.returncode, 0, p.stderr + p.stdout)
            self.assertIn("-R", n_call)
            self.assertIn("1 passed", p.stdout)

    def test_empty_selection_exit1(self):
        with tempfile.TemporaryDirectory() as td:
            p, _ = self._run_n(
                ["--profile", "focused", "--regex", "^zzz"], td)
            self.assertEqual(p.returncode, 1, p.stderr)
            self.assertIn("选中集为空", p.stderr)


class ContractEnforcementTests(unittest.TestCase):
    def test_require_count(self):
        with tempfile.TemporaryDirectory() as td:
            build, _ = make_env(td)
            p = run_profile(["--build-dir", build, "--profile", "smoke",
                             "--require-count", "3"], td)
            self.assertEqual(p.returncode, 1, p.stderr)
            self.assertIn("要求 3", p.stderr)

    def test_require_file_missing(self):
        with tempfile.TemporaryDirectory() as td:
            build, _ = make_env(td)
            req = os.path.join(td, "req.txt")
            with open(req, "w") as f:
                f.write("a.smoke\nnot.registered\n")
            p = run_profile(["--build-dir", build, "--profile", "smoke",
                             "--require-file", req], td)
            self.assertEqual(p.returncode, 1, p.stderr)
            self.assertIn("not.registered", p.stderr)

    def test_notrun_mismatch_fails(self):
        # 实跑集比注册集少一个 → 注册集≠实跑集 FATAL
        with tempfile.TemporaryDirectory() as td:
            build, _ = make_env(td)
            p = run_profile(["--build-dir", build, "--profile", "smoke"],
                            td, extra_env={"CTEST_FAKE_NOTRUN": "b.smoke"})
            self.assertEqual(p.returncode, 1, p.stderr)
            self.assertIn("实跑集与注册集不一致", p.stderr)

    def test_skipped_fails(self):
        with tempfile.TemporaryDirectory() as td:
            build, _ = make_env(td)
            p = run_profile(["--build-dir", build, "--profile", "smoke"],
                            td, extra_env={"CTEST_FAKE_SKIP": "1"})
            self.assertEqual(p.returncode, 1, p.stderr)
            self.assertIn("skipped", p.stderr)

    def test_ctest_failure_propagates(self):
        # 假 ctest 恒 0；改成让实跑 ctest 非零：-N 阶段不受影响，实跑阶段
        # 注入退出。最简验证：脚本 set -e 下 ctest 非零直接冒泡非零退出。
        with tempfile.TemporaryDirectory() as td:
            bin_dir = os.path.join(td, "bin")
            build = os.path.join(td, "build")
            os.makedirs(bin_dir); os.makedirs(build)
            ctest = os.path.join(bin_dir, "ctest")
            with open(ctest, "w") as f:
                f.write('#!/usr/bin/env bash\n'
                        'if [[ "$*" == *"-N"* ]]; then\n'
                        '  echo \'{"tests":[{"name":"x"}]}\'\n'
                        'else\n'
                        '  exit 7\n'
                        'fi\n')
            os.chmod(ctest, 0o755)
            env = dict(ENV)
            env["PATH"] = bin_dir + os.pathsep + env.get("PATH", "")
            env["CTEST_ARGV_LOG"] = os.path.join(td, "argv.log")
            p = subprocess.run(["bash", SCRIPT, "--build-dir", build,
                                "--profile", "smoke"], env=env,
                               capture_output=True, text=True)
            self.assertEqual(p.returncode, 7, p.stderr + p.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
