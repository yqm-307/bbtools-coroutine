#!/usr/bin/env python3
"""Format or check C/C++ files using the repository's clang-format style."""

import argparse
from glob import escape
import os
from pathlib import Path
import re
import subprocess
import sys
from tempfile import TemporaryDirectory
from xml.etree import ElementTree


CPP_EXTENSIONS = {'.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx', '.inl', '.ipp'}


def clang_format(command, path, source):
    output = subprocess.check_output([*command, f'--assume-filename={path}'], input=source)
    # Unlike formatted stdout, replacement XML distinguishes ignored paths from clean files.
    if not output:
        return None
    replacements = ElementTree.fromstring(output)
    parts, cursor = [], 0
    for replacement in replacements:
        offset = int(replacement.attrib['offset'])
        parts.extend((source[cursor:offset], (replacement.text or '').encode()))
        cursor = offset + int(replacement.attrib['length'])
    parts.append(source[cursor:])
    return b''.join(parts)


def is_ignored_without_worktree_file(binary, path):
    path = path.absolute()
    for parent in path.parents:
        config = parent / '.clang-format-ignore'
        if config.is_file():
            break
    else:
        return False

    # clang-format only matches ignore rules for existing files. Give it a
    # placeholder outside the worktree; the actual source still comes from Git.
    with TemporaryDirectory(prefix='format-cpp-') as tmp:
        mirror = Path(tmp).resolve()
        probe = mirror / path.relative_to(path.anchor)
        probe.parent.mkdir(parents=True)
        probe.touch()
        prefix = escape(os.fsencode(mirror))
        rules = []
        for line in config.read_bytes().split(b'\n'):
            rule = line.strip()
            if not rule or rule.startswith(b'#'):
                continue
            negated = rule.startswith(b'!')
            pattern = rule[1:].lstrip() if negated else rule
            if pattern:
                if not pattern.startswith(b'/'):
                    pattern = os.path.normpath(os.path.join(os.fsencode(config.parent), pattern))
                rules.append((b'!' if negated else b'') + prefix + pattern)
        (mirror / '.clang-format-ignore').write_bytes(b'\n'.join(rules))
        return bool(subprocess.check_output([binary, '--list-ignored', '--', str(probe)]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument('--all', action='store_true', help='select all tracked C/C++ files')
    selection.add_argument('--staged', action='store_true', help='check C/C++ blobs in the Git index')
    parser.add_argument('--check', action='store_true', help='check without changing files or the Git index')
    args = parser.parse_args()
    if args.staged and not args.check:
        parser.error('--staged requires --check')
    root = Path(os.fsdecode(subprocess.check_output(['git', 'rev-parse', '--show-toplevel'])).rstrip('\n'))
    os.chdir(root)
    if not any(Path(name).is_file() for name in ('.clang-format', '_clang-format')):
        parser.error('missing repository .clang-format or _clang-format')

    binary = os.environ.get('CLANG_FORMAT') or 'clang-format'
    wanted = os.environ.get('CLANG_FORMAT_VERSION') or '23.1.3'
    version = subprocess.check_output([binary, '--version'], text=True).strip()
    actual = re.search(r'\d+\.\d+\.\d+', version)
    if actual is None or actual[0] != wanted:
        parser.error(f'need clang-format {wanted}; got {version}')
    command = [binary, '--style=file', '--Werror', '--output-replacements-xml']

    def paths(*args):
        data = subprocess.check_output(['git', *args, '-z'])
        return {Path(os.fsdecode(path)) for path in data.split(b'\0') if path}

    staged_links = set()
    if args.all:
        candidates = paths('ls-files')
    elif args.staged:
        candidates = paths('diff', '--cached', '--name-only', '--diff-filter=ACMR')
        entries = subprocess.check_output(['git', 'ls-files', '--stage', '-z'])
        staged_links = {Path(os.fsdecode(entry.partition(b'\t')[2]))
                        for entry in entries.split(b'\0') if entry.startswith(b'120000 ')}
    else:
        candidates = paths('diff', '--cached', '--name-only') | paths('ls-files', '-mo', '--exclude-standard')
    files = sorted(path for path in candidates if path.suffix in CPP_EXTENSIONS and 'third-party' not in path.parts)
    checked, changed = 0, {}
    for path in files:
        if args.staged:
            if path in staged_links:
                raise ValueError(f'{path}: staged symbolic link is not a C/C++ source blob; check it with --check --all')
            if not path.is_file() and is_ignored_without_worktree_file(binary, path):
                continue
            source = subprocess.check_output(['git', 'cat-file', 'blob', f':{path}'])
        else:
            resolved = path.resolve()
            if resolved != path.absolute():
                if not args.check:
                    continue
                if not resolved.is_relative_to(root) or not resolved.is_file():
                    raise ValueError(f'{path}: symbolic link does not resolve to a file inside the repository')
            if not path.is_file():
                if args.check and args.all:
                    raise ValueError(f'{path}: tracked file is missing from the worktree')
                continue
            source = path.read_bytes()
        formatted = clang_format(command, path, source)
        if formatted is None:
            continue
        checked += 1
        if formatted == source:
            continue
        if args.check:
            print(f'{path}: C/C++ format differs', file=sys.stderr)
        else:
            # Nested lambdas can reflow again after their surrounding call is formatted.
            second = clang_format(command, path, formatted)
            if second != formatted and clang_format(command, path, second) != second:
                raise ValueError(f'{path}: clang-format did not stabilize after two passes')
            formatted = second
        changed[path] = formatted

    if args.check and changed:
        print(f'Format failed: {len(changed)}/{checked} C/C++ files differ', file=sys.stderr)
        return 1
    for path, formatted in changed.items():
        path.write_bytes(formatted)
    print(f'Format OK: {checked} C/C++ files checked, {len(changed)} updated')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, subprocess.CalledProcessError, ElementTree.ParseError, ValueError) as error:
        sys.exit(f'format-cpp: {error}')
