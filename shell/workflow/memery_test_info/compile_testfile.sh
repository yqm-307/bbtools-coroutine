#!/bin/bash
# 遗留入口转发（#379）：保留旧调用签名，内部转到内存检测专用构建入口。
#   旧：compile_testfile.sh <workpath>
#   新：scripts/ci/build_memcheck_target.sh <workpath>
# 旧实现用 `rm -rf build` + `make`，既删除调用者产物又无有界并发、无 ccache；
# 新入口只在独立 build-memcheck/ 里构建唯一目标 mem_check_test。
set -euo pipefail
workpath=${1:-$(pwd)}
exec "$(cd "$(dirname "$0")/../../.." && pwd)/scripts/ci/build_memcheck_target.sh" "${workpath}"
