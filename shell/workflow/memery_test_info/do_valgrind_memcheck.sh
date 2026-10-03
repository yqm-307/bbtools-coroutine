#!/bin/bash
# 遗留入口转发（#379）：保留旧调用签名，内部转到 fail-closed 的 memcheck 值守入口。
#   旧：do_valgrind_memcheck.sh <checkexec> <outputfile>
#   新：scripts/ci/run_memcheck.py --binary <checkexec>
#        --report-dir <outputfile 所在目录> --log-name <outputfile 基名>
#
# 旧实现只跑 valgrind、不判读结果也不设退出码：任何泄漏/崩溃/空日志都会“绿”。
# 新入口对缺工具、空/缺日志、Fatal、崩溃、超时、无 ERROR SUMMARY、
# definite/indirect 泄漏、未完成标记一律 FAIL 并返回非 0；不使用抑制文件。
set -euo pipefail

checkexec=${1:?usage: do_valgrind_memcheck.sh <checkexec> <outputfile>}
outputfile=${2:?usage: do_valgrind_memcheck.sh <checkexec> <outputfile>}

repo_root=$(cd "$(dirname "$0")/../../.." && pwd)
report_dir=$(dirname "${outputfile}")
log_name=$(basename "${outputfile}")
log_name=${log_name%.log}

exec python3 "${repo_root}/scripts/ci/run_memcheck.py" \
    --binary "${checkexec}" \
    --report-dir "${report_dir}" \
    --log-name "${log_name}"
