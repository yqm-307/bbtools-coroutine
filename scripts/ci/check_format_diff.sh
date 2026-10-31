#!/usr/bin/env bash
# check_format_diff.sh —— 只检查 git diff 中 C/C++ 改动行的 clang-format 违规。
#
# 用法：
#   tools/check_format_diff.sh <base>          # 比较 <base>...HEAD（三方点语义），并覆盖未提交工作树/暂存改动
#   tools/check_format_diff.sh <base> --staged # 只检查暂存区
#
# 实现：优先 git-clang-format（uv wheel 附带，内部调 clang-format-diff.py），
# 退回 clang-format-diff.py + git diff -U0。只读、不改工作区。
# 退出码：0 无违规；1 有违规或用法错误。
set -euo pipefail

BASE="${1:-}"
if [ -z "$BASE" ]; then
    echo "usage: $0 <base-commit-or-ref> [--staged]" >&2
    exit 1
fi
MODE="${2:-}"

RE_EXTS='.*\.(c|cc|cpp|cxx|h|hh|hpp|hxx|inl|ipp)$'
# 未跟踪文件参与格式检查的扩展名（与 RE_EXTS 对应的 shell glob）。
EXT_GLOBS=(c cc cpp cxx h hh hpp hxx inl ipp)

# 允许 CLANG_FORMAT 覆盖二进制名（CI 上可用系统包）。
CF_BIN="${CLANG_FORMAT:-clang-format}"
GCF_BIN="$(command -v git-clang-format || true)"
CFD_BIN="$(command -v clang-format-diff.py || command -v clang-format-diff || true)"

if [ -n "$GCF_BIN" ]; then
    # git-clang-format 需要 style 解析自仓库根 .clang-format；--diff 只输出差异。
    if [ "$MODE" = "--staged" ]; then
        OUT="$(git clang-format --diff --staged --extensions 'cc,cpp,cxx,h,hh,hpp,hxx,inl,ipp' 2>&1 || true)"
    else
        OUT="$(git clang-format --diff "$BASE" --extensions 'cc,cpp,cxx,h,hh,hpp,hxx,inl,ipp' 2>&1 || true)"
    fi
    if printf '%s' "$OUT" | grep -qE '^(diff|@@|[+-][^+-])'; then
        printf '%s\n' "$OUT"
        echo "check_format_diff: formatting violations found" >&2
        exit 1
    fi
    # clang-format 输出 "no modified files to format" / "clang-format did not modify any files"
    exit 0
fi

if [ -n "$CFD_BIN" ]; then
    # 收集待检查 diff；默认模式除 ${BASE}...HEAD 提交差异外，还必须覆盖
    # 相对 HEAD 的未提交工作树/暂存改动与未跟踪 C/C++ 文件，
    # 否则脏工作树会在空提交 diff 上被误判为格式通过。
    DIFF=""
    if [ "$MODE" = "--staged" ]; then
        DIFF="$(git diff -U0 --no-color --cached --diff-filter=ACMR)"
    else
        DIFF="$(git diff -U0 --no-color "${BASE}...HEAD" --diff-filter=ACMR)"
        WT="$(git diff -U0 --no-color HEAD --diff-filter=ACMR)"
        if [ -n "$WT" ]; then
            DIFF="${DIFF:+$DIFF$'\n'}${WT}"
        fi
        # 未跟踪文件：git diff 不覆盖，用 /dev/null 一侧手工生成等价 diff。
        while IFS= read -r f; do
            case "$f" in
                *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.hxx|*.inl|*.ipp) ;;
                *) continue ;;
            esac
            [ -f "$f" ] || continue
            U="$(git diff --no-index -U0 -- /dev/null "$f" || true)"
            if [ -n "$U" ]; then
                DIFF="${DIFF:+$DIFF$'\n'}${U}"
            fi
        done < <(git ls-files --others --exclude-standard)
    fi
    if [ -z "$DIFF" ]; then
        exit 0
    fi
    OUT="$(printf '%s\n' "$DIFF" | "$CFD_BIN" -p1 -regex "$RE_EXTS" -binary "$CF_BIN" || true)"
    if [ -n "$OUT" ]; then
        printf '%s\n' "$OUT"
        echo "check_format_diff: formatting violations found" >&2
        exit 1
    fi
    exit 0
fi

echo "check_format_diff: neither git-clang-format nor clang-format-diff found on PATH" >&2
exit 1
