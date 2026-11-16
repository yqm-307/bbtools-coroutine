#!/usr/bin/env python3
"""`scripts/ci/prepare_boost.sh` 跨 run 源码缓存（`BOOST_SOURCE_CACHE`）行为测试。

只测**缓存分支的决策与校验**，不联网、不构建 Boost：把真实脚本复制到临时目录，
仅把锁定的 `BOOST_SHA256` 换成合成归档的 sha256，再用 PATH shim 的 `curl` 与归档内
的 `bootstrap.sh`/`b2` 桩驱动完整流程。合成的下载与安装**不是**真实下载/构建证据；
真实 hosted 冷热对照由独立验证节点执行，本文件不据此宣称提速。

覆盖：命中（校验通过 → 跳过下载）/ 缺失 / 损坏（校验失败 → 重下并重新校验）/
未启用（无环境变量 → 行为与改动前一致）/ 缓存目录或条目为符号链接（忽略缓存）/
下载内容不符锁定 SHA256（fail-closed 退出 3，且不回写缓存）。

会被正式 `ci.yml` 的 `python3 -m unittest discover -s scripts/ci -p 'test_*.py'` 自动发现。

运行：PYTHONDONTWRITEBYTECODE=1 python3 scripts/ci/test_ci_cache.py
"""
from __future__ import annotations

import hashlib
import io
import os
import re
import subprocess
import tarfile
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REAL_SCRIPT = os.path.join(HERE, "prepare_boost.sh")
PINNED_SHA256 = "5e93d582aff26868d581a52ae78c7d8edf3f3064742c6e77901a1f18a437eea9"
ARCHIVE_NAME = "boost_1_90_0.tar.gz"

# 归档内的最小桩：bootstrap 记录 prefix，b2 把脚本随后校验的两个标记写进 prefix。
BOOTSTRAP_STUB = """#!/usr/bin/env bash
set -euo pipefail
for arg in "$@"; do
    case "$arg" in
        --prefix=*) printf '%s\\n' "${arg#--prefix=}" > .prefix ;;
    esac
done
"""
B2_STUB = """#!/usr/bin/env bash
set -euo pipefail
prefix="$(cat .prefix)"
mkdir -p "$prefix/include/boost" "$prefix/lib"
printf '#define BOOST_VERSION 109000\\n' > "$prefix/include/boost/version.hpp"
: > "$prefix/lib/libboost_context.so"
"""
CURL_SHIM = """#!/usr/bin/env bash
set -euo pipefail
out=""
while [ $# -gt 0 ]; do
    case "$1" in
        -o) out="$2"; shift 2 ;;
        *) shift ;;
    esac
done
[ -n "$out" ] || { echo "shim-curl: 缺少 -o" >&2; exit 2; }
printf '%s\\n' called >> "$CURL_LOG"
cp "$CURL_PAYLOAD" "$out"
"""

# 模拟「缓存条目校验通过、但复制到工作目录的副本被替换」：只对指定源写坏字节，
# 其余调用转发给真 cp（用例见 test_corrupted_working_copy_falls_back_to_download）。
CP_SHIM = """#!/usr/bin/env bash
set -euo pipefail
args=()
for arg in "$@"; do
    case "$arg" in
        --) ;;
        -*) ;;
        *) args+=("$arg") ;;
    esac
done
n=${#args[@]}
src="${args[$((n - 2))]:-}"
dst="${args[$((n - 1))]:-}"
if [ -n "$dst" ] && [ "$src" = "${CP_CORRUPT_SRC:-}" ]; then
    printf 'corrupted copy\\n' > "$dst"
    exit 0
fi
real=/bin/cp
[ -x "$real" ] || real=/usr/bin/cp
exec "$real" "$@"
"""


def _read_bytes(path: str) -> bytes:
    with open(path, "rb") as handle:
        return handle.read()


def _read_text(path: str) -> str:
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def _write_bytes(path: str, payload: bytes) -> None:
    with open(path, "wb") as handle:
        handle.write(payload)


def _sha256(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _write_tar(path: str, members: dict[str, tuple[bytes, int]]) -> None:
    with tarfile.open(path, "w:gz") as tar:
        for name, (payload, mode) in members.items():
            info = tarfile.TarInfo(f"boost_1_90_0/{name}")
            info.size = len(payload)
            info.mode = mode
            info.mtime = 0
            tar.addfile(info, io.BytesIO(payload))


def _fixture_archive(path: str, marker: bytes = b"") -> str:
    _write_tar(
        path,
        {
            "bootstrap.sh": (BOOTSTRAP_STUB.encode() + marker, 0o755),
            "b2": (B2_STUB.encode(), 0o755),
        },
    )
    return _sha256(path)


def _write_exec(path: str, content: str) -> None:
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(content)
    os.chmod(path, 0o755)


class BoostSourceCacheTests(unittest.TestCase):
    def setUp(self) -> None:
        self._td = tempfile.TemporaryDirectory()
        self.tmp = self._td.name
        self.addCleanup(self._td.cleanup)

        shim = os.path.join(self.tmp, "bin")
        os.makedirs(shim)
        _write_exec(os.path.join(shim, "curl"), CURL_SHIM)
        self.shim_dir = shim

        # 合成归档与「下载得到的字节」（默认与归档逐字节相同）。
        self.good_archive = os.path.join(self.tmp, "good.tar.gz")
        self.good_sha = _fixture_archive(self.good_archive)
        self.bad_payload = os.path.join(self.tmp, "bad.tar.gz")
        self.bad_sha = _fixture_archive(self.bad_payload, marker=b"# corrupt\n")

        self.curl_log = os.path.join(self.tmp, "curl.log")
        self.payload = os.path.join(self.tmp, "payload.tar.gz")
        self._set_payload(self.good_archive)

        # 只替换锁定 SHA256 常量，其余逐字节保留真实脚本逻辑。
        source = _read_text(REAL_SCRIPT)
        patched, count = re.subn(
            r'(?m)^BOOST_SHA256="[0-9a-f]{64}"$',
            f'BOOST_SHA256="{self.good_sha}"',
            source,
            count=1,
        )
        self.assertEqual(count, 1, "真实脚本必须保留单行 BOOST_SHA256 常量")
        self.script = os.path.join(self.tmp, "prepare_boost.sh")
        with open(self.script, "w", encoding="utf-8") as handle:
            handle.write(patched)
        self.cache_dir = os.path.join(self.tmp, "src-cache")

    def _set_payload(self, path: str) -> None:
        _write_bytes(self.payload, _read_bytes(path))

    def _run(self, prefix_name: str = "prefix", cache: str | None = None,
             path_prefix: str | None = None, env_extra: dict[str, str] | None = None):
        prefix = os.path.join(self.tmp, prefix_name)
        env = dict(os.environ)
        first = path_prefix or self.shim_dir
        env["PATH"] = first + os.pathsep + env.get("PATH", "")
        env["TMPDIR"] = os.path.join(self.tmp, "tmp")
        os.makedirs(env["TMPDIR"], exist_ok=True)
        env["CURL_LOG"] = self.curl_log
        env["CURL_PAYLOAD"] = self.payload
        env.pop("BOOST_SOURCE_CACHE", None)
        env.pop("CP_CORRUPT_SRC", None)
        if cache is not None:
            env["BOOST_SOURCE_CACHE"] = cache
        if env_extra:
            env.update(env_extra)
        return prefix, subprocess.run(
            ["bash", self.script, prefix, "--jobs", "1"],
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )

    def _curl_calls(self) -> int:
        if not os.path.exists(self.curl_log):
            return 0
        with open(self.curl_log, encoding="utf-8") as handle:
            return len([line for line in handle if line.strip()])

    def _assert_built(self, prefix: str, proc) -> None:
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertTrue(os.path.isfile(os.path.join(prefix, "include/boost/version.hpp")))
        self.assertTrue(os.path.isfile(os.path.join(prefix, "lib/libboost_context.so")))

    def test_real_script_keeps_locked_sha256(self) -> None:
        # 合成归档只用于行为测试；真实脚本必须仍是锁定版本/SHA256。
        source = _read_text(REAL_SCRIPT)
        self.assertIn(f'BOOST_SHA256="{PINNED_SHA256}"', source)
        self.assertIn('BOOST_VERSION="1.90.0"', source)

    def test_cache_hit_skips_download(self) -> None:
        os.makedirs(self.cache_dir)
        _write_bytes(os.path.join(self.cache_dir, ARCHIVE_NAME),
                     _read_bytes(self.good_archive))
        prefix, proc = self._run(cache=self.cache_dir)
        self._assert_built(prefix, proc)
        self.assertIn("命中源码缓存", proc.stdout)
        self.assertEqual(self._curl_calls(), 0, "命中且校验通过时不得下载")

    def test_cache_missing_downloads_and_writes_back(self) -> None:
        os.makedirs(self.cache_dir)
        prefix, proc = self._run(cache=self.cache_dir)
        self._assert_built(prefix, proc)
        self.assertEqual(self._curl_calls(), 1)
        self.assertIn("已回写源码缓存", proc.stdout)
        cached = os.path.join(self.cache_dir, ARCHIVE_NAME)
        self.assertTrue(os.path.isfile(cached))
        self.assertEqual(_sha256(cached), self.good_sha)

    def test_corrupt_cache_falls_back_and_refreshes_entry(self) -> None:
        os.makedirs(self.cache_dir)
        cached = os.path.join(self.cache_dir, ARCHIVE_NAME)
        _write_bytes(cached, b"not a boost archive")
        prefix, proc = self._run(cache=self.cache_dir)
        self._assert_built(prefix, proc)
        self.assertIn("源码缓存 sha256 不符", proc.stdout)
        self.assertEqual(self._curl_calls(), 1, "损坏条目必须回落下载")
        self.assertEqual(_sha256(cached), self.good_sha, "重新校验通过后才回写")

    def test_corrupted_working_copy_falls_back_to_download(self) -> None:
        # 缓存条目本身校验通过，但复制到工作目录的副本被换掉：判定 hit 必须基于
        # 副本字节（解包用的是副本），副本不符就是没命中，回落下载。
        os.makedirs(self.cache_dir)
        shim = os.path.join(self.tmp, "cp-shim")
        os.makedirs(shim)
        _write_exec(os.path.join(shim, "cp"), CP_SHIM)
        cached = os.path.join(self.cache_dir, ARCHIVE_NAME)
        _write_bytes(cached, _read_bytes(self.good_archive))
        prefix, proc = self._run(
            cache=self.cache_dir,
            path_prefix=shim + os.pathsep + self.shim_dir,
            env_extra={"CP_CORRUPT_SRC": cached},
        )
        self._assert_built(prefix, proc)
        self.assertIn("源码缓存副本 sha256 不符", proc.stdout)
        self.assertNotIn("命中源码缓存", proc.stdout)
        self.assertEqual(self._curl_calls(), 1, "副本不符必须回落下载")
        self.assertEqual(_sha256(cached), self.good_sha, "回写必须来自校验通过的下载字节")

    def test_disabled_without_env_var_keeps_legacy_behaviour(self) -> None:
        os.makedirs(self.cache_dir)
        prefix, proc = self._run(cache=None)
        self._assert_built(prefix, proc)
        self.assertEqual(self._curl_calls(), 1)
        self.assertNotIn("源码缓存", proc.stdout)
        self.assertEqual(os.listdir(self.cache_dir), [], "未启用时不得写缓存")

    def test_symlinked_cache_dir_is_ignored(self) -> None:
        real_cache = os.path.join(self.tmp, "real-cache")
        os.makedirs(real_cache)
        _write_bytes(os.path.join(real_cache, ARCHIVE_NAME),
                     _read_bytes(self.good_archive))
        link = os.path.join(self.tmp, "cache-link")
        os.symlink(real_cache, link)
        prefix, proc = self._run(cache=link)
        self._assert_built(prefix, proc)
        self.assertIn("BOOST_SOURCE_CACHE 是符号链接", proc.stdout)
        self.assertEqual(self._curl_calls(), 1, "符号链接缓存目录不得被复用")

    def test_symlinked_cache_entry_is_ignored(self) -> None:
        os.makedirs(self.cache_dir)
        os.symlink(self.good_archive, os.path.join(self.cache_dir, ARCHIVE_NAME))
        prefix, proc = self._run(cache=self.cache_dir)
        self._assert_built(prefix, proc)
        self.assertIn("源码缓存条目是符号链接", proc.stdout)
        self.assertEqual(self._curl_calls(), 1)
        self.assertTrue(os.path.islink(os.path.join(self.cache_dir, ARCHIVE_NAME)),
                        "不得跟随符号链接写入缓存")

    @unittest.skipIf(hasattr(os, "geteuid") and os.geteuid() == 0,
                     "root 会绕过目录写权限限制")
    def test_unwritable_cache_dir_does_not_break_build(self) -> None:
        # 缓存故障（只读目录）必须退回冷路径，不得让构建失败。
        os.makedirs(self.cache_dir)
        os.chmod(self.cache_dir, 0o555)
        self.addCleanup(os.chmod, self.cache_dir, 0o755)
        prefix, proc = self._run(cache=self.cache_dir)
        self._assert_built(prefix, proc)
        self.assertEqual(self._curl_calls(), 1)
        self.assertIn("缓存回写失败", proc.stdout)
        self.assertEqual(os.listdir(self.cache_dir), [])

    def test_download_failing_locked_sha256_is_fatal_and_not_cached(self) -> None:
        os.makedirs(self.cache_dir)
        self._set_payload(self.bad_payload)
        prefix, proc = self._run(cache=self.cache_dir)
        self.assertEqual(proc.returncode, 3, proc.stdout + proc.stderr)
        self.assertIn("sha256 不符", proc.stderr)
        self.assertFalse(os.path.exists(prefix))
        self.assertEqual(os.listdir(self.cache_dir), [], "未通过校验的字节不得进缓存")


if __name__ == "__main__":
    unittest.main()
