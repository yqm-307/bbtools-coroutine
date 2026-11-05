#!/usr/bin/env bash
# 本地与 CI 共用的文档导出入口：Doxygen 只解析一次，同时产出两份文档。
#   $DOCS_OUT/site/       人看：HTML（Pages 根）；其下 ai/ 是同一份 AI 版
#   $DOCS_OUT/ai/         AI 看：分层 Markdown（L0 index.md → L1 modules → L2 symbols）
#   $DOCS_OUT/warnings.log  Doxygen 告警（缺注释等）
# 环境变量（均可选）：
#   DOCS_OUT              输出目录，默认 <repo>/build-docs（已被 build*/ 忽略）
#   DOXYGEN_AWESOME_CSS   doxygen-awesome.css 的路径；缺省用 Doxygen 默认样式
#   DOCS_WERROR=1         有任何 Doxygen 告警即失败（缺注释门禁；默认只告警）
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
cfg=scripts/docs/modules.json

command -v doxygen >/dev/null || { echo "缺少 doxygen" >&2; exit 2; }
command -v dot >/dev/null || echo "警告: 缺少 graphviz(dot)，类图将缺失" >&2

export DOCS_OUT=${DOCS_OUT:-$root/build-docs}
export DOCS_PROJECT=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["project"])' "$cfg")
export DOCS_VERSION=${DOCS_VERSION:-$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)}
export DOXYGEN_AWESOME_CSS=${DOXYGEN_AWESOME_CSS:-}

echo "doxygen $(doxygen --version) · project=$DOCS_PROJECT · version=$DOCS_VERSION"
rm -rf "$DOCS_OUT"
mkdir -p "$DOCS_OUT"

doxygen scripts/docs/Doxyfile
python3 -I scripts/docs/export_ai_docs.py "$DOCS_OUT/xml" "$DOCS_OUT/ai" "$cfg"

# Pages 站点：HTML 在根，AI 版放 /ai/；.nojekyll 防止 Pages 把 .md 当 Jekyll 源处理
mkdir -p "$DOCS_OUT/site"
cp -r "$DOCS_OUT/html/." "$DOCS_OUT/site/"
cp -r "$DOCS_OUT/ai" "$DOCS_OUT/site/ai"
touch "$DOCS_OUT/site/.nojekyll"

warn=$(grep -c . "$DOCS_OUT/warnings.log" 2>/dev/null || true)
echo "doxygen 告警: ${warn:-0} 条（$DOCS_OUT/warnings.log）"
if [ "${DOCS_WERROR:-0}" = 1 ] && [ "${warn:-0}" -gt 0 ]; then
    echo "DOCS_WERROR=1：存在 Doxygen 告警，失败" >&2
    exit 1
fi
