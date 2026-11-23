#!/usr/bin/env bash
# check_format.sh —— 用本仓格式化入口检查 C/C++（只读，不改文件）。
#
# 用法（从仓库任意目录运行均可）：
#   bash scripts/ci/check_format.sh            # 检查全部已跟踪的 C/C++ 文件（CI 用）
#   bash scripts/ci/check_format.sh --staged   # 只检查暂存区里的 C/C++ 文件（提交前用）
#
# 提交前钩子（本地一次性安装，不入仓）：
#   printf '#!/bin/sh\nexec bash scripts/ci/check_format.sh --staged\n' > .git/hooks/pre-commit
#   chmod +x .git/hooks/pre-commit
#
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

case "$#:${1:-}" in
    0:) scope=--all ;;
    1:--staged) scope=--staged ;;
    *) printf 'Usage: %s [--staged]\n' "$0" >&2; exit 2 ;;
esac
exec python3 -I scripts/ci/format_cpp.py --check "$scope"
