#!/bin/bash
# 安装 bbtools-coroutine 头文件与共享库。
#
# Issue #12 安装所有权约定（P2 起）：
#   本仓发布 bbt/coroutine/**、bbt/pollevent/** 及 bbt/core 下的固定子集：
#     Attribute.hpp、Define.hpp、clock/、errcode/、thread/、
#     util/Assert.hpp、util/Result.hpp、log/DebugPrint.hpp
#   bbt/core 的其余路径归 bbtools-infra 发布。本脚本只增删上面列出的
#   自有路径，禁止 rm -rf ${installpath}/bbt/core 整目录覆盖。

set -euo pipefail

installpath="/usr/local/include"
libpath="/usr/local/lib"

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# mktemp 暂存目录统一登记，任何失败/中断路径都经 EXIT trap 清理；
# 配合 set -e，暂存或复制失败即退出，不会继续走到 rm -rf 目标目录。
_stage_dirs=()
_cleanup_stages() {
    if ((${#_stage_dirs[@]})); then
        rm -rf "${_stage_dirs[@]}"
    fi
}
trap _cleanup_stages EXIT

# 整目录安装（仅限本仓独占的目录）：剔除编译单元后整体替换目标子目录
install_dir() {
    local rel="$1"                      # 例：bbt/pollevent
    local dst="${installpath}/${rel}"
    local tmp
    tmp=$(mktemp -d)
    _stage_dirs+=("${tmp}")
    mkdir -p "${tmp}/hdr"
    cp -rf "${repo_root}/${rel}/." "${tmp}/hdr/"
    find "${tmp}/hdr" \( -name '*.cc' -o -name '*.cpp' -o -name '*.cxx' \) -type f -delete
    [ -d "${tmp}/hdr" ] || return 1
    sudo mkdir -p "$(dirname "${dst}")"
    sudo rm -rf "${dst}"
    sudo mv "${tmp}/hdr" "${dst}"
}

# 逐文件安装：只覆盖本仓持有的单文件，不动同目录下归 infra 的其它文件
install_file() {
    local rel="$1"
    sudo mkdir -p "${installpath}/$(dirname "${rel}")"
    sudo install -m 0644 "${repo_root}/${rel}" "${installpath}/${rel}"
}

# ── 本仓独占目录 ──
install_dir bbt/coroutine
install_dir bbt/pollevent
install_dir bbt/core/clock
install_dir bbt/core/errcode
install_dir bbt/core/thread

# ── bbt/core 下与 infra 共目录的单文件 ──
install_file bbt/core/Attribute.hpp
install_file bbt/core/Define.hpp
install_file bbt/core/util/Assert.hpp
install_file bbt/core/util/Result.hpp
install_file bbt/core/log/DebugPrint.hpp

sudo install -m 0755 "${repo_root}/build/lib/libbbt_coroutine.so" "${libpath}/libbbt_coroutine.so"

echo "安装完毕"
