#!/bin/bash
# 内存检测专用构建入口：只配置并构建 mem_check_test 目标。
#
# 与普通 CI 的差异（本文件只服务 memery_test_info.yml，不动 unit_test.yml / build.sh）：
# - 只构建一个目标，避免整仓编译；RelWithDebInfo 提供调试符号，便于 memcheck 反查分配栈；
# - NEED_VALGRIND=ON 把协程可用栈区间接入 Valgrind（默认生产构建不依赖 Valgrind，
#   头文件缺失时 CMake 直接 FATAL_ERROR，不静默降级）；
# - PROFILE 保持 OFF：完成协议只用栈池账目（与 PROFILE 无关），不使用 PROFILE=OFF
#   时会恒 0 的 Profiler 计数，因此这里不需要、也不依赖 PROFILE=ON。
#
# 构建目录独立于普通 build/，不删除调用者已有产物（默认 <workpath>/build-memcheck）。
# 并发显式有界，遵循本仓本地约定 nproc/4（最小 2）。valgrind.h 位置可用
# BBT_VALGRIND_INCLUDE 指定（私有解包场景）。
set -euo pipefail

workpath=${1:-$(pwd)}
if [ ! -d "${workpath}" ]; then
    echo "workpath ${workpath} not exist" >&2
    exit 1
fi
workpath=$(cd "${workpath}" && pwd)

build_dir=${MEMCHECK_BUILD_DIR:-${workpath}/build-memcheck}
jobs=${MEMCHECK_BUILD_JOBS:-$(( $(nproc) / 4 ))}
[ "${jobs}" -lt 2 ] && jobs=2

cmake_args=(
    -S "${workpath}" -B "${build_dir}" -G Ninja
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
    -DNEED_VALGRIND=ON
    -DNEED_BENCHMARK=ON -DNEED_TEST=OFF -DNEED_EXAMPLE=OFF -DNEED_DEBUG=OFF -DPROFILE=OFF
)
if [ -n "${BBT_VALGRIND_INCLUDE:-}" ]; then
    cmake_args+=(-DBBT_VALGRIND_INCLUDE="${BBT_VALGRIND_INCLUDE}")
fi

cmake "${cmake_args[@]}"
cmake --build "${build_dir}" --target mem_check_test --parallel "${jobs}"

exec_path="${build_dir}/bin/benchmark_test/mem_check_test"
if [ ! -x "${exec_path}" ]; then
    echo "内存检测目标未生成：${exec_path}" >&2
    exit 1
fi
echo "mem_check_test 构建完成：${exec_path}"
