#!/usr/bin/env python3
"""Check that staged formatting honors ignore rules for index-only blobs."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(os.environ.get('FORMAT_CPP', Path(__file__).with_name('format_cpp.py'))).resolve()
BAD_SOURCE = 'int  add( int a,int b ){ return a+b; }\n'
IGNORED = Path('bbt/coroutine/utils/lockfree/ignored.h')
CHECKED = Path('bbt/coroutine/checked.h')


class StagedIgnoreTest(unittest.TestCase):
    def test_staged_ignored_blob_without_worktree_copy_is_not_reported(self):
        # CI installs the pinned formatter in the format job, not the job that discovers these tests.
        binary = os.environ.get('CLANG_FORMAT') or 'clang-format'
        wanted = os.environ.get('CLANG_FORMAT_VERSION') or '23.1.3'
        if shutil.which(binary) is None:
            self.skipTest(f'clang-format {wanted} is not installed')
        version = subprocess.check_output([binary, '--version'], text=True)
        actual = re.search(r'\d+\.\d+\.\d+', version)
        if actual is None or actual[0] != wanted:
            self.skipTest(f'need clang-format {wanted}; got {version.strip()}')

        temp = tempfile.TemporaryDirectory(prefix='format-ignore-')
        self.addCleanup(temp.cleanup)
        repo = Path(temp.name)

        def git(*args):
            return subprocess.run(['git', *args], cwd=repo, check=True, capture_output=True).stdout

        def check_staged():
            result = subprocess.run([sys.executable, str(SCRIPT), '--check', '--staged'], cwd=repo,
                                    capture_output=True, text=True)
            reported = set(re.findall(r'^(.+): C/C\+\+ format differs$', result.stderr, re.MULTILINE))
            return result.returncode, reported, result.stderr

        git('init', '-q')
        (repo / '.clang-format').write_text('BasedOnStyle: LLVM\n')
        (repo / '.clang-format-ignore').write_text('bbt/coroutine/utils/lockfree/*\n')
        for path in (IGNORED, CHECKED):
            (repo / path).parent.mkdir(parents=True, exist_ok=True)
            (repo / path).write_text(BAD_SOURCE)
        git('add', '--', IGNORED.as_posix(), CHECKED.as_posix())

        # Removing the worktree copies must not change which staged blobs are checked.
        expected = (1, {CHECKED.as_posix()})
        code, reported, stderr = check_staged()
        self.assertEqual((code, reported), expected, stderr)
        index_before = git('ls-files', '--stage')
        for path in (IGNORED, CHECKED):
            (repo / path).unlink()

        code, reported, stderr = check_staged()
        self.assertEqual((code, reported), expected, stderr)
        self.assertEqual(git('ls-files', '--stage'), index_before, '--check modified the Git index')
        for path in (IGNORED, CHECKED):
            self.assertFalse((repo / path).exists(), '--check recreated a worktree file')


if __name__ == '__main__':
    unittest.main()
