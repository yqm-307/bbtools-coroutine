#!/usr/bin/env python3
"""check_docs.py 的单元测试：在临时目录构造正反例。"""

import contextlib
import io
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_docs  # noqa: E402


def reasons(violations):
    return [r for _, _, r in violations]


def has(violations, needle):
    return any(needle in r for r in reasons(violations))


class DocsCheckCase(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix='check-docs-test-')
        self.addCleanup(shutil.rmtree, self.root, True)
        self.write('README.md', '# t\n')

    def write(self, rel, content):
        path = os.path.join(self.root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w', encoding='utf-8') as f:
            f.write(content)

    def check(self, readme_max=200):
        return check_docs.check(self.root, readme_max)


class TestLinks(DocsCheckCase):
    def test_broken_link(self):
        self.write('docs/guides/a.md', '[x](missing.md)\n')
        v = self.check()
        self.assertTrue(has(v, '相对链接目标不存在: missing.md'))
        self.assertEqual(v[0][0], 'docs/guides/a.md')
        self.assertEqual(v[0][1], 1)

    def test_ok_links_anchors_and_external(self):
        self.write('docs/a.md',
                   '[ok](b.md#frag) [ext](https://x.example/1) '
                   '[anchor](#self) [mail](mailto:a@b.c)\n')
        self.write('docs/b.md', '# b\n')
        self.assertEqual(self.check(), [])

    def test_anchors_and_code_fences(self):
        self.write('docs/a.md',
                   '[a](#frag) [b](b.md#frag)\n'
                   '```\n[not-a-link](gone.md)\n```\n')
        self.write('docs/b.md', '# b\n## frag\n')
        self.assertEqual(self.check(), [])

    def test_reference_def_link(self):
        self.write('docs/a.md', '[use it][r]\n\n[r]: nowhere.md\n')
        v = self.check()
        self.assertTrue(has(v, 'nowhere.md'))


class TestFilenames(DocsCheckCase):
    def test_issue_number_filename(self):
        self.write('docs/issue-123-thing.md', 'x\n')
        v = self.check()
        self.assertTrue(has(v, '文件名含过程编号'))
        self.assertTrue(has(v, 'Issue/PR 编号'))

    def test_review_code_dirname(self):
        self.write('docs/f2-notes/a.md', 'x\n')
        v = self.check()
        self.assertTrue(has(v, '目录名含过程编号'))

    def test_date_and_version_names(self):
        self.write('docs/guide-2026-10-30.md', 'x\n')
        self.write('docs/plan-v1.md', 'x\n')
        v = self.check()
        self.assertTrue(has(v, '日期'))
        self.assertTrue(has(v, '版本标记'))

    def test_adr_prefix_exempt(self):
        self.write('docs/decisions/0001-use-boost.md', 'x\n')
        self.assertEqual(self.check(), [])

    def test_adr_body_still_checked(self):
        self.write('docs/decisions/0001-issue-42.md', 'x\n')
        self.assertTrue(has(self.check(), '文件名含过程编号'))

    def test_noise_outside_docs_unchecked(self):
        self.write('notes/issue-9.md', 'x\n')
        self.assertEqual(self.check(), [])


class TestReadme(DocsCheckCase):
    def test_readme_over_limit(self):
        self.write('README.md', '\n'.join('l%d' % i for i in range(60)) + '\n')
        v = self.check(readme_max=50)
        self.assertTrue(has(v, '超过上限 50'))
        self.assertEqual(v[0][1], 51)

    def test_readme_at_limit_ok(self):
        self.write('README.md', '\n'.join('l%d' % i for i in range(50)) + '\n')
        self.assertEqual(self.check(readme_max=50), [])


class TestAdrNumbers(DocsCheckCase):
    def test_duplicate_adr(self):
        self.write('docs/decisions/0001-a.md', 'x\n')
        self.write('docs/decisions/0001-b.md', 'x\n')
        v = self.check()
        self.assertTrue(has(v, 'ADR 编号 0001 重复'))

    def test_unique_adrs_ok(self):
        self.write('docs/decisions/0001-a.md', 'x\n')
        self.write('docs/decisions/0002-b.md', 'x\n')
        self.assertEqual(self.check(), [])


class TestMain(DocsCheckCase):
    def test_exit_codes(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc_ok = check_docs.main(['--root', self.root])
        self.assertEqual(rc_ok, 0)
        self.write('docs/issue-1.md', 'x\n')
        with contextlib.redirect_stdout(out):
            rc_bad = check_docs.main(['--root', self.root])
        self.assertEqual(rc_bad, 1)
        self.assertIn('docs/issue-1.md:1:', out.getvalue())
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            rc_err = check_docs.main(['--root', '/no/such/dir'])
        self.assertEqual(rc_err, 2)


if __name__ == '__main__':
    unittest.main()
