#!/usr/bin/env python3
"""run_memcheck.py 的契约测试：验证值守入口对每条反例都 fail-closed。

定位说明（重要）：
- 本测试用“真实可执行脚本”充当 valgrind，通过真实 subprocess 与真实退出码驱动
  run_memcheck.py，覆盖它自己的判定与退出码逻辑；不是对“检测能力”的 mock。
- 因此本测试的 PASS 用例只证明**判定逻辑**正确，不构成对真实 Valgrind 检测通过的
  证据；真实检测 PASS 必须在装有 valgrind+libc6-dbg 的 runner 上实跑。
- 不修改 run_memcheck.py 的判定口径来迎合用例；反例都是“应当 FAIL”的行为。

运行：python3 scripts/ci/test_memcheck_contract.py
（也随 `python3 -m unittest discover -s scripts/ci -p 'test_*.py'` 执行）
"""

from __future__ import annotations

import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "scripts" / "ci" / "run_memcheck.py"

sys.path.insert(0, str(REPO_ROOT / "scripts" / "ci"))
import run_memcheck  # noqa: E402

FAKE_VALGRIND = r'''#!/usr/bin/env python3
import os, signal, sys, time, pathlib

args = sys.argv[1:]
if "--version" in args:
    print(os.environ.get("FAKE_VG_VERSION", "valgrind-3.26.0"))
    sys.exit(int(os.environ.get("FAKE_VG_VERSION_RC", "0")))

log = None
for a in args:
    if a.startswith("--log-file="):
        log = a.split("=", 1)[1]
guests = [a for a in args if not a.startswith("-")]
guest = guests[-1] if guests else ""

def write_log(text):
    if log:
        pathlib.Path(log).write_text(text)

is_probe = os.path.basename(guest) == "true"
if is_probe and os.environ.get("FAKE_VG_PROBE_FATAL") == "1":
    write_log("valgrind:  Fatal error at startup: cannot find ld-linux symbols\n")
    sys.exit(1)
if is_probe:
    write_log("==0== ERROR SUMMARY: 0 errors from 0 contexts\n")
    sys.exit(int(os.environ.get("FAKE_VG_PROBE_RC", "0")))

if os.environ.get("FAKE_VG_SLEEP"):
    time.sleep(float(os.environ["FAKE_VG_SLEEP"]))

write_log(os.environ.get("FAKE_VG_LOG", ""))
sys.stdout.write(os.environ.get("FAKE_VG_STDOUT", ""))
sys.stderr.write(os.environ.get("FAKE_VG_STDERR", ""))
# FAKE_VG_SIGNAL=1 时真的被 SIGSEGV 杀死：子进程 rc 为负（-11），
# 用于覆盖 run_memcheck.py 的 rc<0 崩溃分支（sys.exit(-11) 只返回 245，不是信号）
if os.environ.get("FAKE_VG_SIGNAL"):
    sys.stdout.flush()
    sys.stderr.flush()
    os.kill(os.getpid(), signal.SIGSEGV)
sys.exit(int(os.environ.get("FAKE_VG_RC", "0")))
'''

CLEAN_LOG = """==1== LEAK SUMMARY:
==1==    definitely lost: 0 bytes in 0 blocks
==1==    indirectly lost: 0 bytes in 0 blocks
==1==      possibly lost: 4,096 bytes in 2 blocks
==1==    still reachable: 18,432 bytes in 9 blocks
==1== ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)
"""

DONE_OK = ("BBT_MEMCHECK_DONE inner=10000/10000 lock=4000/4000 chan_read=500000/500000 "
           "writer_done=50/50 failures=0/0 runtime_drained=1/1 "
           "stack_pool_drained=1/1 (released=3 alloc=0 cur=0)\n")


class MemcheckContractTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.fake = self.tmp / "valgrind"
        self.fake.write_text(FAKE_VALGRIND)
        self.fake.chmod(self.fake.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
        # 一个“真实存在且可执行”的被测目标：直接用 /bin/true 占位，判定不看它是不是 mem_check_test
        self.binary = self.tmp / "target"
        self.binary.write_text("#!/bin/sh\n")
        self.binary.chmod(0o755)
        # 判定逻辑夹具自给 Boost 版本头，不依赖 ARC 的 /opt/boost 或宿主安装；
        # 仍走真实 preflight，不能把此夹具当作真实 Boost/Valgrind 检测证据。
        self.boost_include = self.tmp / "boost-include"
        (self.boost_include / "boost").mkdir(parents=True)
        (self.boost_include / "boost" / "version.hpp").write_text(
            '#define BOOST_LIB_VERSION "1_90"\n')

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _run(self, env_extra: dict, extra_args: list[str] | None = None):
        env = dict(os.environ)
        env["BBT_BOOST_INCLUDE_ROOT"] = str(self.boost_include)
        env.update({k: str(v) for k, v in env_extra.items()})
        args = [sys.executable, str(RUNNER),
                "--valgrind", str(self.fake),
                "--binary", str(self.binary),
                "--report-dir", str(self.tmp / "out"),
                "--timeout", str(env_extra.get("ARG_TIMEOUT", 30))]
        if extra_args:
            args += extra_args
        return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              env=env, timeout=120)

    def _expect_fail(self, name: str, env_extra: dict, extra_args=None):
        proc = self._run(env_extra, extra_args)
        out = proc.stdout.decode(errors="replace")
        self.assertNotEqual(proc.returncode, 0, f"{name} 应 FAIL 却返回 0：\n{out}")
        self.assertIn("MEMCHECK VERDICT: FAIL", out, f"{name} 未打印 FAIL：\n{out}")
        return out

    # ---- 正向：判定逻辑（非真实检测证据）----
    def test_clean_run_passes_judging_logic(self):
        proc = self._run({"FAKE_VG_LOG": CLEAN_LOG, "FAKE_VG_STDOUT": DONE_OK,
                          "FAKE_VG_STDERR": "[bbt-mc] ordinary diagnostic\n", "FAKE_VG_RC": 0})
        out = proc.stdout.decode(errors="replace")
        self.assertEqual(proc.returncode, 0, f"干净用例应 PASS：\n{out}")
        self.assertIn("MEMCHECK VERDICT: PASS", out)
        summary = json.loads((self.tmp / "out" / "summary.json").read_text())
        self.assertEqual(summary["verdict"], "PASS")
        self.assertIsNone(summary["suppressions"])   # 契约：不使用抑制文件

    # ---- 反例：每条都必须 FAIL 且真实退出码非 0 ----
    def test_missing_valgrind_fails(self):
        # 覆盖 valgrind 存在性：--valgrind 指向不存在路径
        args = [sys.executable, str(RUNNER), "--valgrind", str(self.tmp / "nope"),
                "--binary", str(self.binary), "--report-dir", str(self.tmp / "o2")]
        proc = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("valgrind-present", proc.stdout.decode(errors="replace"))

    def test_version_probe_failure_fails(self):
        self._expect_fail("version rc!=0",
                          {"FAKE_VG_VERSION_RC": 3, "FAKE_VG_LOG": CLEAN_LOG,
                           "FAKE_VG_STDOUT": DONE_OK})

    def test_loader_symbols_fatal_fails(self):
        # 启动期 Fatal（模拟缺 ld-linux/libc6-dbg）：probe 即致命
        self._expect_fail("loader fatal",
                          {"FAKE_VG_PROBE_FATAL": 1, "FAKE_VG_LOG": CLEAN_LOG,
                           "FAKE_VG_STDOUT": DONE_OK})

    def test_missing_binary_fails(self):
        args = [sys.executable, str(RUNNER), "--valgrind", str(self.fake),
                "--binary", str(self.tmp / "no-such-bin"),
                "--report-dir", str(self.tmp / "o3")]
        proc = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("binary-present", proc.stdout.decode(errors="replace"))

    def test_timeout_fails(self):
        self._expect_fail("timeout",
                          {"FAKE_VG_SLEEP": 5, "FAKE_VG_LOG": CLEAN_LOG,
                           "FAKE_VG_STDOUT": DONE_OK, "ARG_TIMEOUT": 1})

    def test_nonzero_rc_fails(self):
        self._expect_fail("rc=42", {"FAKE_VG_RC": 42, "FAKE_VG_LOG": CLEAN_LOG,
                                    "FAKE_VG_STDOUT": DONE_OK})

    def test_crash_signal_fails(self):
        # 真实崩溃：fake valgrind 自身被 SIGSEGV 杀死 → subprocess rc=-11，覆盖 rc<0 分支
        out = self._expect_fail("crash", {"FAKE_VG_SIGNAL": 1, "FAKE_VG_LOG": CLEAN_LOG,
                                          "FAKE_VG_STDOUT": DONE_OK})
        self.assertIn("信号终止", out)
        self.assertIn("rc=-11", out)

    # ---- 完成协议：缺键/重复键/缩水/多标记都必须 FAIL ----
    def test_missing_required_counters_fail(self):
        # 只凑出一个自洽计数（缺其余必需键）也不许 PASS（真实反例：截断输出）
        for key in ("lock=4000/4000 ", "runtime_drained=1/1 ",
                    "stack_pool_drained=1/1 ", "failures=0/0 "):
            with self.subTest(key=key):
                bad = DONE_OK.replace(key, "")
                out = self._expect_fail(f"missing {key}",
                                        {"FAKE_VG_LOG": CLEAN_LOG, "FAKE_VG_STDOUT": bad})
                self.assertIn("缺少必需计数", out)

    def test_only_single_selfconsistent_counter_fails(self):
        # 评审反例：stdout 只有一个 inner=10000/10000 也被判 PASS
        self._expect_fail("single counter", {"FAKE_VG_LOG": CLEAN_LOG,
                                             "FAKE_VG_STDOUT": "BBT_MEMCHECK_DONE inner=10000/10000\n"})

    def test_shrunk_workload_fails(self):
        # 键齐全且 num==total，但工作量缩水（1/1、0/0）同样不许 PASS
        for old, new in (("inner=10000/10000", "inner=1/1"),
                         ("chan_read=500000/500000", "chan_read=0/0"),
                         ("runtime_drained=1/1", "runtime_drained=0/0"),
                         ("stack_pool_drained=1/1", "stack_pool_drained=0/0"),
                         ("failures=0/0", "failures=1/1")):
            with self.subTest(new=new):
                bad = DONE_OK.replace(old, new)
                self._expect_fail(f"shrunk {new}",
                                  {"FAKE_VG_LOG": CLEAN_LOG, "FAKE_VG_STDOUT": bad})

    def test_duplicate_counter_key_fails(self):
        bad = DONE_OK.replace("inner=10000/10000", "inner=1/1 inner=10000/10000")
        out = self._expect_fail("duplicate key", {"FAKE_VG_LOG": CLEAN_LOG, "FAKE_VG_STDOUT": bad})
        self.assertIn("重复", out)

    def test_conflicting_done_markers_fail(self):
        # 两条互相冲突的完成标记 → 无法判定即 FAIL
        out = DONE_OK + "BBT_MEMCHECK_DONE inner=1/1 lock=4000/4000 chan_read=500000/500000 " \
                        "writer_done=50/50 failures=0/0 runtime_drained=1/1 " \
                        "stack_pool_drained=1/1\n"
        self._expect_fail("two markers", {"FAKE_VG_LOG": CLEAN_LOG, "FAKE_VG_STDOUT": out})

    # ---- 报告：前置失败也要落 FAIL summary，且不得残留上一轮 PASS ----
    def test_preflight_failure_does_not_run_workload_and_writes_fail_summary(self):
        args = [sys.executable, str(RUNNER), "--valgrind", str(self.fake),
                "--binary", str(self.tmp / "no-such-bin"),
                "--report-dir", str(self.tmp / "o4"), "--log-name", "memcheck"]
        report_dir = self.tmp / "o4"
        report_dir.mkdir(parents=True)
        # 造上一轮 PASS 残留
        (report_dir / "summary.json").write_text(json.dumps({"verdict": "PASS"}))
        (report_dir / "memcheck.log").write_text("==1== ERROR SUMMARY: 0 errors from 0 contexts\n")
        (report_dir / "memcheck.stdout.log").write_text(DONE_OK)
        proc = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        self.assertNotEqual(proc.returncode, 0)
        summary = json.loads((report_dir / "summary.json").read_text())
        self.assertEqual(summary["verdict"], "FAIL")
        self.assertFalse((report_dir / "memcheck.log").exists(), "旧 valgrind 日志必须被清掉")
        self.assertFalse((report_dir / "memcheck.stdout.log").exists(), "旧 stdout 必须被清掉")

    def test_preflight_ok_runs_workload(self):
        # 对照：前置全绿才可能启动 workload（否则上一条测试的“不写入新日志”无从区分）
        proc = self._run({"FAKE_VG_LOG": "==1== ERROR SUMMARY: 3 errors from 1 contexts\n",
                          "FAKE_VG_STDOUT": DONE_OK})
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("ERROR SUMMARY", (self.tmp / "out" / "memcheck.log").read_text())

    def test_empty_log_fails(self):
        self._expect_fail("empty log", {"FAKE_VG_LOG": "   \n", "FAKE_VG_STDOUT": DONE_OK})

    def test_missing_error_summary_fails(self):
        log = ("==1==    definitely lost: 0 bytes in 0 blocks\n"
               "==1==    indirectly lost: 0 bytes in 0 blocks\n")
        self._expect_fail("no ERROR SUMMARY", {"FAKE_VG_LOG": log, "FAKE_VG_STDOUT": DONE_OK})

    def test_definitely_lost_fails(self):
        log = CLEAN_LOG.replace("definitely lost: 0 bytes in 0 blocks",
                                "definitely lost: 1,744 bytes in 3 blocks")
        self._expect_fail("definitely lost", {"FAKE_VG_LOG": log, "FAKE_VG_STDOUT": DONE_OK})

    def test_indirectly_lost_fails(self):
        log = CLEAN_LOG.replace("indirectly lost: 0 bytes in 0 blocks",
                                "indirectly lost: 172,032,000 bytes in 999 blocks")
        self._expect_fail("indirectly lost", {"FAKE_VG_LOG": log, "FAKE_VG_STDOUT": DONE_OK})

    def test_error_summary_nonzero_fails(self):
        log = CLEAN_LOG.replace("ERROR SUMMARY: 0 errors from 0 contexts",
                                "ERROR SUMMARY: 7 errors from 3 contexts")
        self._expect_fail("errors>0", {"FAKE_VG_LOG": log, "FAKE_VG_STDOUT": DONE_OK})

    def test_missing_done_marker_fails(self):
        self._expect_fail("no DONE marker", {"FAKE_VG_LOG": CLEAN_LOG,
                                             "FAKE_VG_STDOUT": "[bbt-mc] stage1 nested done\n"})

    def test_counter_mismatch_fails(self):
        bad = DONE_OK.replace("inner=10000/10000", "inner=9999/10000")
        self._expect_fail("counter mismatch", {"FAKE_VG_LOG": CLEAN_LOG, "FAKE_VG_STDOUT": bad})

    def test_explicit_failed_marker_fails(self):
        out = DONE_OK + "BBT_MEMCHECK_FAILED 完成协议不达标（business=0 drain=1）\n"
        self._expect_fail("FAILED marker", {"FAKE_VG_LOG": CLEAN_LOG, "FAKE_VG_STDOUT": out})

    def test_explicit_failed_marker_in_stderr_fails(self):
        failed = "BBT_MEMCHECK_FAILED 完成协议不达标（business=0 drain=1）\n"
        out = self._expect_fail("stderr FAILED marker",
                                {"FAKE_VG_LOG": CLEAN_LOG, "FAKE_VG_STDOUT": DONE_OK,
                                 "FAKE_VG_STDERR": failed, "FAKE_VG_RC": 0})
        self.assertIn("程序自报未完成", out)

    def test_check_env_detects_missing_tools(self):
        # 工作流前置校验：缺 valgrind 时 check-env 也必须 FAIL（不允许“跳过”）
        args = [sys.executable, str(RUNNER), "--check-env",
                "--valgrind", str(self.tmp / "absent")]
        proc = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("valgrind-present", proc.stdout.decode(errors="replace"))

    # ---- 纯单元：Boost 根目录发现（含 /opt/boost/include 候选）----
    def test_find_boost_root_prefers_env_override(self):
        root = self.tmp / "boostroot"
        (root / "boost").mkdir(parents=True)
        (root / "boost" / "version.hpp").write_text('#define BOOST_LIB_VERSION "1_90"\n')
        os.environ["BBT_BOOST_INCLUDE_ROOT"] = str(root)
        try:
            found, detail = run_memcheck.find_boost_root()
        finally:
            os.environ.pop("BBT_BOOST_INCLUDE_ROOT", None)
        self.assertEqual(found, str(root))
        self.assertIn("1_90", detail)

    def test_find_boost_root_reports_missing(self):
        saved = run_memcheck.BOOST_ROOT_CANDIDATES
        run_memcheck.BOOST_ROOT_CANDIDATES = (str(self.tmp / "no-boost"),)
        try:
            found, detail = run_memcheck.find_boost_root()
        finally:
            run_memcheck.BOOST_ROOT_CANDIDATES = saved
        self.assertIsNone(found)
        self.assertIn("未找到 Boost", detail)


if __name__ == "__main__":
    unittest.main()
