#!/usr/bin/env bash
# 构建并安装 bbtools-coroutine 到用户前缀（默认 ~/.local）。
# 本仓只安装自己拥有的 bbt/coroutine、bbt/pollevent 与 bbt/core 子集。
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
prefix="${BBT_INSTALL_PREFIX:-${HOME}/.local}"
build_dir="${BBT_BUILD_DIR:-${repo_root}/build-install}"
jobs="${BBT_JOBS:-2}"

cmake -S "${repo_root}" -B "${build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DNEED_BENCHMARK=OFF -DNEED_EXAMPLE=OFF -DNEED_TEST=OFF -DNEED_DEBUG=OFF
cmake --build "${build_dir}" --parallel "${jobs}"

install -d "${prefix}/include" "${prefix}/lib"

copy_headers() {
    local rel="$1"
    while IFS= read -r -d '' file; do
        local target="${prefix}/include/${file#"${repo_root}/"}"
        install -D -m 0644 "${file}" "${target}"
    done < <(find "${repo_root}/${rel}" -type f \( -name '*.h' -o -name '*.hh' -o -name '*.hpp' \) -print0)
}

copy_headers bbt/coroutine
copy_headers bbt/pollevent
for file in \
    bbt/core/Attribute.hpp bbt/core/Define.hpp \
    bbt/core/clock bbt/core/errcode bbt/core/thread \
    bbt/core/util/Assert.hpp bbt/core/util/Result.hpp \
    bbt/core/log/DebugPrint.hpp; do
    if [ -d "${repo_root}/${file}" ]; then
        copy_headers "${file}"
    else
        install -D -m 0644 "${repo_root}/${file}" "${prefix}/include/${file}"
    fi
done

install -m 0755 "${build_dir}/lib/libbbt_coroutine.so" "${prefix}/lib/libbbt_coroutine.so"
printf '安装完成：%s\n' "${prefix}"
