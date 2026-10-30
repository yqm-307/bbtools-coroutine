#!/usr/bin/env bash
# test_check_format_diff.sh —— check_format_diff.sh 的最小自测。
#
# 覆盖两个失败模式：
#   1. 无 git-clang-format 时，fallback 必须发现未提交工作树/暂存/未跟踪的格式违规；
#   2. 正常 formatter 路径与 PR 三点 diff 语义不退化。
#
# 运行：bash scripts/ci/test_check_format_diff.sh
# 依赖：git、clang-format、clang-format-diff.py（无则跳过 fallback 用例）。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHECK="${SCRIPT_DIR}/check_format_diff.sh"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAKEBIN="${TMP}/bin"
mkdir -p "$FAKEBIN"

pass=0; fail=0
ok()  { pass=$((pass+1)); echo "PASS: $1"; }
bad() { fail=$((fail+1)); echo "FAIL: $1" >&2; }

# --- 构造最小仓库：1 个已提交的 C++ 文件 + 1 个未提交违规 + 1 个未跟踪违规 ---
REPO="${TMP}/repo"
mkdir -p "$REPO"
git -C "$REPO" init -q
git -C "$REPO" config user.email t@t
git -C "$REPO" config user.name t
printf 'BasedOnStyle: Google\n' > "$REPO/.clang-format"
printf 'int main() { return 0; }\n' > "$REPO/a.cc"
git -C "$REPO" add -A
git -C "$REPO" commit -qm init
BASE="$(git -C "$REPO" rev-parse HEAD)"

# 未提交修改：工作树中写入一处格式违规
printf 'int main( ){return 0;}\n' > "$REPO/a.cc"
# 未跟踪新文件：同样带格式违规
printf 'int  f(){return 1;}\n' > "$REPO/b.cc"

CFD_BIN="$(command -v clang-format-diff.py || command -v clang-format-diff || true)"
GCF_BIN="$(command -v git-clang-format || true)"

# 用例 1：fallback（无 git-clang-format）必须对工作树改动返回 rc=1。
# 做法：构造只含 git/clang-format/clang-format-diff 的 PATH。
if [ -n "$CFD_BIN" ]; then
  ln -sf "$(command -v git)"          "$FAKEBIN/git"
  ln -sf "$(command -v clang-format)" "$FAKEBIN/clang-format"
  ln -sf "$CFD_BIN"                   "$FAKEBIN/$(basename "$CFD_BIN")"
  ln -sf "$(command -v bash)"         "$FAKEBIN/bash"
  # 屏蔽 git-clang-format
  run_fallback() { PATH="$FAKEBIN" bash "$CHECK" "$@"; }
  if (cd "$REPO" && run_fallback "$BASE" >/dev/null 2>&1); then
    bad "fallback reported clean on dirty worktree"
  else
    ok "fallback flags uncommitted worktree violation (rc=1)"
  fi
  # --staged：暂存区为空也应干净（脏的是工作树，不是 index）
  if (cd "$REPO" && run_fallback "$BASE" --staged >/dev/null 2>&1); then
    ok "fallback --staged clean on empty index"
  else
    bad "fallback --staged false-positive on empty index"
  fi
else
  echo "SKIP: clang-format-diff not found, fallback case skipped" >&2
fi

# 用例 2：正常 git-clang-format 路径应同样发现工作树违规（不退化）。
if [ -n "$GCF_BIN" ]; then
  if (cd "$REPO" && bash "$CHECK" "$BASE" >/dev/null 2>&1); then
    bad "git-clang-format path reported clean on dirty worktree"
  else
    ok "git-clang-format path flags worktree violation (rc=1)"
  fi
else
  echo "SKIP: git-clang-format not found, formatter-path case skipped" >&2
fi

# 用例 3：干净仓库（HEAD 之上无改动）应 rc=0。
CLEAN="${TMP}/clean"
mkdir -p "$CLEAN"
git -C "$CLEAN" init -q
git -C "$CLEAN" config user.email t@t
git -C "$CLEAN" config user.name t
printf 'BasedOnStyle: Google\n' > "$CLEAN/.clang-format"
printf 'int main() { return 0; }\n' > "$CLEAN/a.cc"
git -C "$CLEAN" add -A
git -C "$CLEAN" commit -qm init
CB="$(git -C "$CLEAN" rev-parse HEAD)"
if [ -n "$CFD_BIN" ]; then
  if (cd "$CLEAN" && run_fallback "$CB" >/dev/null 2>&1); then
    ok "fallback clean on clean worktree"
  else
    bad "fallback false-positive on clean worktree"
  fi
fi

echo "----"
echo "passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
