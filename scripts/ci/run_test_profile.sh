#!/usr/bin/env bash
# 统一测试执行入口：按 profile 选集 → 非空校验 → ctest 实跑 → JUnit →
# 校验注册集=实跑集、skipped/failure/error 一律判失败。
# 三仓（bbt-framework / bbtools-infra / bbtools-coroutine）共用同一份契约，
# 判据与 run_ctest_label.sh / 现役 ci.yml 同口径：
#   -j1 --no-tests=error --output-on-failure，--timeout 仅对无 TIMEOUT 属性的用例生效。
#
# 用法：
#   run_test_profile.sh --build-dir <dir> --profile smoke [--junit <file>] [--timeout 120]
#   run_test_profile.sh --build-dir <dir> --profile focused --regex '^chan|^poller' ...
#   run_test_profile.sh --build-dir <dir> --profile full [--include-labels 'live|perf'] ...
#   run_test_profile.sh --build-dir <dir> --profile focused --label-expr 'unit' ...
#
# profile 选集契约（注册集内 label 由 CTest 属性决定）：
#   smoke    → -L smoke
#   focused  → 必须给 --regex <expr>（或显式 --label-expr），否则 FATAL；
#              二者都给时取交集（ctest -L + -R 语义）。
#   full     → 默认排除 live|perf（无真实服务/性能开关的本地默认路径）；
#              --include-labels 'live|perf' 可从排除集中放回。
#
# 通用选项（三仓一致）：
#   --build-dir D        构建目录（必填，内含 CTest 注册信息）
#   --profile P          smoke | focused | full（必填）
#   --label-expr E       追加/覆盖 label 选择（focused 下等价于附加 -L 过滤）
#   --regex R            测试名正则（focused 必填；smoke/full 下附加 -R 过滤）
#   --include-labels E   full 专用：从默认排除集 'live|perf' 中放回的 label 正则
#   --timeout S          ctest --timeout（默认 120，仅对无 TIMEOUT 属性用例生效）
#   --junit F            JUnit 输出路径；不给时默认 <build-dir>/junit-<profile>.xml
#   --require-count N    选集数必须 >= N
#   --require-file F     必选测试名清单（一名字/行，# 注释忽略），缺项 FATAL
#   --repeat SPEC        透传 ctest --repeat（如 'until-fail:5'）
#
# 退出码：0 全过；1 选集/一致性/用例判定失败；2 参数错误；其余=ctest 退出码。
set -euo pipefail

BUILD_DIR=""
PROFILE=""
LABEL_EXPR=""
REGEX=""
INCLUDE_LABELS=""
TIMEOUT="120"
JUNIT=""
REQUIRE_COUNT=""
REQUIRE_FILE=""
REPEAT=""

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --profile) PROFILE="$2"; shift 2 ;;
        --label-expr) LABEL_EXPR="$2"; shift 2 ;;
        --regex) REGEX="$2"; shift 2 ;;
        --include-labels) INCLUDE_LABELS="$2"; shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --junit) JUNIT="$2"; shift 2 ;;
        --require-count) REQUIRE_COUNT="$2"; shift 2 ;;
        --require-file) REQUIRE_FILE="$2"; shift 2 ;;
        --repeat) REPEAT="$2"; shift 2 ;;
        *) echo "[test-profile] FATAL: 未知参数 $1" >&2; exit 2 ;;
    esac
done

[ -n "$BUILD_DIR" ] || { echo "[test-profile] FATAL: 缺 --build-dir" >&2; exit 2; }
[ -d "$BUILD_DIR" ] || { echo "[test-profile] FATAL: 构建目录不存在: $BUILD_DIR" >&2; exit 2; }
[ -n "$PROFILE" ] || { echo "[test-profile] FATAL: 缺 --profile" >&2; exit 2; }

# ctest --output-junit 相对 --test-dir 解析，而后置 JUnit 校验相对 CWD 解析：
# 归一化为绝对路径，防止相对 build-dir 把 junit 写进 <build>/<build>/。
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
[ -n "$JUNIT" ] && case "$JUNIT" in /*) ;; *) JUNIT="$(pwd)/$JUNIT" ;; esac

EXCLUDE_LABELS=""
case "$PROFILE" in
    smoke)
        [ -z "$LABEL_EXPR" ] && LABEL_EXPR="smoke"
        ;;
    focused)
        if [ -z "$REGEX" ] && [ -z "$LABEL_EXPR" ]; then
            echo "[test-profile] FATAL: focused profile 必须给 --regex 或 --label-expr" >&2
            exit 2
        fi
        ;;
    full)
        EXCLUDE_LABELS="live|perf"
        ;;
    *)
        echo "[test-profile] FATAL: 未知 profile '$PROFILE'（smoke|focused|full）" >&2
        exit 2
        ;;
esac

# full 的 include-labels 从排除集放回：live|perf 去掉命中的 token。
if [ "$PROFILE" = "full" ] && [ -n "$INCLUDE_LABELS" ]; then
    EXCLUDE_LABELS="$(EXCLUDE_LABELS="$EXCLUDE_LABELS" INCLUDE_LABELS="$INCLUDE_LABELS" python3 - <<'PY'
import os, re
excl = [t for t in os.environ["EXCLUDE_LABELS"].split("|") if t]
inc = os.environ["INCLUDE_LABELS"]
kept = [t for t in excl if not re.fullmatch(inc, t)]
print("|".join(kept))
PY
)"
fi

SELECT_ARGS=()
[ -n "$LABEL_EXPR" ] && SELECT_ARGS+=(-L "$LABEL_EXPR")
[ -n "$EXCLUDE_LABELS" ] && SELECT_ARGS+=(-LE "$EXCLUDE_LABELS")
[ -n "$REGEX" ] && SELECT_ARGS+=(-R "$REGEX")

REGISTERED="$BUILD_DIR/.test-profile-registered.json"
ctest --test-dir "$BUILD_DIR" -N --show-only=json-v1 \
    "${SELECT_ARGS[@]+"${SELECT_ARGS[@]}"}" > "$REGISTERED"

SELECTED_JSON="$BUILD_DIR/.test-profile-selected.json"
python3 - "$REGISTERED" "$SELECTED_JSON" <<'PY'
import json, sys
data = json.load(open(sys.argv[1]))
names = [t["name"] for t in data["tests"]]
json.dump(names, open(sys.argv[2], "w"))
print(len(names))
PY
COUNT="$(python3 -c "import json; print(len(json.load(open('$SELECTED_JSON'))))")"
if [ "$COUNT" -eq 0 ]; then
    echo "[test-profile] FATAL: profile=$PROFILE 选中集为空（label/regex 未注册任何用例）" >&2
    exit 1
fi
if [ -n "$REQUIRE_COUNT" ] && [ "$COUNT" -lt "$REQUIRE_COUNT" ]; then
    echo "[test-profile] FATAL: 注册集 $COUNT < 要求 $REQUIRE_COUNT" >&2
    exit 1
fi
if [ -n "$REQUIRE_FILE" ]; then
    python3 - "$SELECTED_JSON" "$REQUIRE_FILE" <<'PY'
import json, sys
selected = set(json.load(open(sys.argv[1])))
required = {line.strip() for line in open(sys.argv[2])
            if line.strip() and not line.lstrip().startswith("#")}
missing = required - selected
if missing:
    raise SystemExit(f"[test-profile] FATAL: 必选注册集缺项: {sorted(missing)}")
print(f"[test-profile] 必选 {len(required)} 项齐全")
PY
fi

[ -z "$JUNIT" ] && JUNIT="$BUILD_DIR/junit-$PROFILE.xml"
REPEAT_ARG=()
[ -n "$REPEAT" ] && REPEAT_ARG=(--repeat "$REPEAT")
RUN_ARGS=(-j1 --no-tests=error --output-on-failure --timeout "$TIMEOUT")
[ -n "$LABEL_EXPR" ] && RUN_ARGS+=(-L "$LABEL_EXPR")
[ -n "$EXCLUDE_LABELS" ] && RUN_ARGS+=(-LE "$EXCLUDE_LABELS")
[ -n "$REGEX" ] && RUN_ARGS+=(-R "$REGEX")

echo "[test-profile] profile=$PROFILE selected=$COUNT junit=$JUNIT"
ctest --test-dir "$BUILD_DIR" "${RUN_ARGS[@]}" \
    --output-junit "$JUNIT" "${REPEAT_ARG[@]+"${REPEAT_ARG[@]}"}"

python3 - "$JUNIT" "$SELECTED_JSON" <<'PY'
import json, sys, xml.etree.ElementTree as ET
selected = set(json.load(open(sys.argv[2])))
cases = list(ET.parse(sys.argv[1]).getroot().iter("testcase"))
ran = {c.get("name") for c in cases}
if len(cases) != len(ran) or ran != selected:
    raise SystemExit(
        f"[test-profile] FATAL: 实跑集与注册集不一致 "
        f"(selected={len(selected)} ran={len(ran)} diff={sorted(selected ^ ran)})")
for tag in ("skipped", "failure", "error"):
    if any(c.find(tag) is not None for c in cases):
        raise SystemExit(f"[test-profile] FATAL: 存在 {tag} 用例")
print(f"[test-profile] {len(cases)} passed, 0 failed, 0 skipped")
PY
