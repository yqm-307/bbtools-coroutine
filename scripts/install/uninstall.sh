#!/usr/bin/env bash
# 卸载本仓安装的用户前缀产物，不需要 root 权限。
set -euo pipefail

prefix="${BBT_INSTALL_PREFIX:-${HOME}/.local}"

rm -rf "${prefix}/include/bbt/coroutine"
rm -rf "${prefix}/include/bbt/pollevent"
rm -rf "${prefix}/include/bbt/core/clock"
rm -rf "${prefix}/include/bbt/core/errcode"
rm -rf "${prefix}/include/bbt/core/thread"
rm -f "${prefix}/include/bbt/core/Attribute.hpp"
rm -f "${prefix}/include/bbt/core/Define.hpp"
rm -f "${prefix}/include/bbt/core/util/Assert.hpp"
rm -f "${prefix}/include/bbt/core/util/Result.hpp"
rm -f "${prefix}/include/bbt/core/log/DebugPrint.hpp"
rm -f "${prefix}/lib/libbbt_coroutine.so"

printf '卸载完成：%s\n' "${prefix}"
