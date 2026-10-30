#!/usr/bin/env python3
"""三仓共用 docs 检查器，仅依赖 Python 标准库。

检查项：
1. 所有 .md 文件中的相对链接目标文件存在（外部链接、纯锚点跳过，
   fenced code block 内的文本不当作链接）；
2. docs/ 下文件名与目录名不含过程编号，规则见
   spec/forbidden-process-noise.md 的文件名正则；
   docs/decisions/NNNN-<slug>.md 的 ADR 编号是唯一例外；
3. 根 README.md 行数不超过 --readme-max；
4. docs/decisions/ 下 ADR 的 NNNN 编号唯一。

输出格式 ``path:line: 原因``（文件级问题记 line 1）。任一违规退出码 1，
参数或 IO 错误退出码 2。
"""

import argparse
import os
import re
import sys
from urllib.parse import unquote

# Markdown 行内链接 [text](target)、![alt](target)，以及引用定义 [label]: target
INLINE_LINK_RE = re.compile(r'!?\[[^\]\n]*\]\(\s*([^)]*?)\s*\)')
REF_DEF_RE = re.compile(r'^\s{0,3}\[[^\]\n]+\]:\s*(\S+)')
FENCE_RE = re.compile(r'^\s*(```|~~~)')
SCHEME_RE = re.compile(r'^[A-Za-z][A-Za-z0-9+.-]*:')
ADR_PREFIX_RE = re.compile(r'^(\d{4})-')

# forbidden-process-noise.md「文件名正则」一节，作用于文件名主体
# （去掉最后一个扩展名）；目录名用全名。
NOISE_RES = [
    (re.compile(r'(?i)(?:^|[-_.])(?:issue|pr)[-_#]?\d+(?=$|[-_.])'),
     'Issue/PR 编号'),
    (re.compile(r'(?i)(?:^|[-_.])(?:f\d+[a-z]?\d*|a\d+[a-z]?\d*|'
                r'r\d+[a-z]?\d*|p\d+[a-z]?\d*)(?=$|[-_.])'),
     '审查/阶段代号'),
    (re.compile(r'(?i)(?:^|[-_.])v\d+(?:\.\d+)*(?=$|[-_.])'),
     '版本标记'),
    (re.compile(r'(?i)(?:^|[-_.])20\d{2}[-_.]?\d{2}[-_.]?\d{2}(?=$|[-_.])'),
     '日期'),
    (re.compile(r'(?i)§\s*\d+(?:\.\d+)*|终评|首审|复审|评审|契约第?[-_]?\d+行'),
     '评审/契约行号用语'),
]

SKIP_DIRS = {'.git', '.svn', '.hg', 'node_modules', '__pycache__'}


def _rel(root, path):
    return os.path.relpath(path, root).replace(os.sep, '/')


def _link_path(raw):
    """把 Markdown 链接目标归一化为本地路径；外部链接/纯锚点返回 None。"""
    s = raw.strip()
    if not s:
        return None
    if s.startswith('<'):
        end = s.find('>')
        s = s[1:end] if end != -1 else s[1:]
    else:
        s = s.split()[0]
    if not s or s.startswith('#') or s.startswith('//'):
        return None
    if SCHEME_RE.match(s):  # http:、mailto:、tel: 等
        return None
    s = s.split('#', 1)[0].split('?', 1)[0]
    if not s:
        return None
    return unquote(s)


def _check_links(root, md_abs, md_rel, violations):
    in_fence = False
    fence_mark = ''
    base = os.path.dirname(md_abs)
    try:
        f = open(md_abs, encoding='utf-8', errors='replace')
    except OSError as e:
        violations.append((md_rel, 1, '无法读取文件: %s' % e))
        return
    with f:
        for lineno, line in enumerate(f, 1):
            m = FENCE_RE.match(line)
            if m:
                mark = m.group(1)
                if not in_fence:
                    in_fence, fence_mark = True, mark
                elif line.lstrip().startswith(fence_mark):
                    in_fence = False
                continue
            if in_fence:
                continue
            targets = [lm.group(1) for lm in INLINE_LINK_RE.finditer(line)]
            rm = REF_DEF_RE.match(line)
            if rm:
                targets.append(rm.group(1))
            for raw in targets:
                target = _link_path(raw)
                if target is None:
                    continue
                dest = os.path.normpath(os.path.join(base, target))
                if not os.path.exists(dest):
                    violations.append(
                        (md_rel, lineno,
                         '相对链接目标不存在: %s' % target))


def _noise_hit(subject):
    for rx, label in NOISE_RES:
        if rx.search(subject):
            return label
    return None


def _check_docs_names(root, violations):
    docs = os.path.join(root, 'docs')
    if not os.path.isdir(docs):
        return
    for dirpath, dirnames, filenames in os.walk(docs):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        rel_dir = os.path.relpath(dirpath, docs)
        in_decisions = rel_dir.split(os.sep)[0] == 'decisions'
        for d in dirnames:
            hit = _noise_hit(d)  # 目录名用全名
            if hit:
                violations.append((
                    _rel(root, os.path.join(dirpath, d)), 1,
                    '目录名含过程编号（%s）: %s' % (hit, d)))
        for name in sorted(filenames):
            subject = os.path.splitext(name)[0]
            if in_decisions:
                m = ADR_PREFIX_RE.match(subject)
                if m:  # ADR 的 NNNN- 前缀例外，编号唯一性另行检查
                    subject = subject[m.end():]
            hit = _noise_hit(subject)
            if hit:
                violations.append((
                    _rel(root, os.path.join(dirpath, name)), 1,
                    '文件名含过程编号（%s）: %s' % (hit, name)))


def _check_adr_numbers(root, violations):
    dec = os.path.join(root, 'docs', 'decisions')
    if not os.path.isdir(dec):
        return
    seen = {}
    for name in sorted(os.listdir(dec)):
        if not name.endswith('.md'):
            continue
        m = ADR_PREFIX_RE.match(name)
        if m:
            seen.setdefault(m.group(1), []).append(name)
    for num, files in sorted(seen.items()):
        for dup in files[1:]:
            violations.append((
                'docs/decisions/' + dup, 1,
                'ADR 编号 %s 重复（另见 docs/decisions/%s）' % (num, files[0])))


def _check_readme(root, readme_max, violations):
    path = os.path.join(root, 'README.md')
    if not os.path.isfile(path):
        return
    with open(path, encoding='utf-8', errors='replace') as f:
        n = sum(1 for _ in f)
    if n > readme_max:
        violations.append((
            'README.md', readme_max + 1,
            'README 行数 %d 超过上限 %d' % (n, readme_max)))


def check(root, readme_max=200):
    """对 root 执行全部检查，返回 (path, line, reason) 列表。"""
    violations = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for name in sorted(filenames):
            if name.endswith('.md'):
                p = os.path.join(dirpath, name)
                _check_links(root, p, _rel(root, p), violations)
    _check_docs_names(root, violations)
    _check_adr_numbers(root, violations)
    _check_readme(root, readme_max, violations)
    violations.sort(key=lambda v: (v[0], v[1]))
    return violations


def main(argv=None):
    ap = argparse.ArgumentParser(description='三仓共用 docs 检查')
    ap.add_argument('--root', required=True, help='仓库根目录')
    ap.add_argument('--readme-max', type=int, default=200,
                    help='README.md 行数上限（默认 200）')
    args = ap.parse_args(argv)
    root = os.path.abspath(args.root)
    if not os.path.isdir(root):
        print('check_docs: 根目录不存在: %s' % args.root, file=sys.stderr)
        return 2
    violations = check(root, args.readme_max)
    for path, line, reason in violations:
        print('%s:%d: %s' % (path, line, reason))
    return 1 if violations else 0


if __name__ == '__main__':
    sys.exit(main())
