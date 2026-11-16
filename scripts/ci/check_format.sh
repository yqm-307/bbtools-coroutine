#!/usr/bin/env bash
# check_format.sh —— 检查 C/C++ 文件是否符合仓库 .clang-format（只读，不改文件）。
#
# 用法（从仓库任意目录运行均可）：
#   bash scripts/ci/check_format.sh            # 检查全部已跟踪的 C/C++ 文件（CI 用）
#   bash scripts/ci/check_format.sh --staged   # 只检查暂存区里的 C/C++ 文件（提交前用）
#
# 提交前钩子（本地一次性安装，不入仓）：
#   printf '#!/bin/sh\nexec bash scripts/ci/check_format.sh --staged\n' > .git/hooks/pre-commit
#   chmod +x .git/hooks/pre-commit
#
# 版本：必须是 .clang-format 头部注明的 clang-format 版本，版本不符直接失败。
# 不同大版本对同一配置的输出不同，放行会让 CI 与本地互相打架。
# 第三方目录由仓库根的 .clang-format-ignore 排除。
# 退出码：0 全部符合；1 有违规、版本不符或工具缺失。
set -euo pipefail

want_version="${CLANG_FORMAT_VERSION:-23.1.3}"
bin="${CLANG_FORMAT:-clang-format}"
cd "$(git rev-parse --show-toplevel)"

if ! command -v "$bin" >/dev/null; then
    echo "check_format: 找不到 $bin（pip install clang-format==$want_version）" >&2
    exit 1
fi
have_version="$("$bin" --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
if [ "$have_version" != "$want_version" ]; then
    echo "check_format: 需要 clang-format $want_version，实际 $have_version" >&2
    exit 1
fi

pattern='\.(c|cc|cpp|cxx|h|hh|hpp|hxx|inl|ipp)$'
if [ "${1:-}" = "--staged" ]; then
    files="$(git diff --cached --name-only --diff-filter=ACMR | grep -E "$pattern" || true)"
else
    files="$(git ls-files | grep -E "$pattern" || true)"
fi
if [ -z "$files" ]; then
    echo "check_format: 没有需要检查的文件"
    exit 0
fi

if ! printf '%s\n' "$files" | xargs "$bin" --dry-run --Werror --style=file 2>&1; then
    echo "check_format: 存在格式违规；本地修复：clang-format -i --style=file <文件>" >&2
    exit 1
fi
echo "check_format: $(printf '%s\n' "$files" | wc -l) 个文件符合 .clang-format"
