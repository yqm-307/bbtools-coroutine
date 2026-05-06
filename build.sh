#!/usr/bin/env bash

set -Eeuo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="${script_dir}"
build_dir="${repo_root}/build"
build_type="Release"
need_example="ON"
need_test="OFF"
need_benchmark="OFF"
need_debug="OFF"
profile="OFF"
debug_info="OFF"
stringent_debug="OFF"
co_trace="OFF"
need_install="ON"
jobs="${BUILD_JOBS:-}"
cmake_args=()

usage() {
    cat <<'EOF'
Usage: ./build.sh [options] [-- <extra-cmake-args...>]

Options:
  --type <type>           CMAKE_BUILD_TYPE, default: Release
  --build-dir <path>      Build directory, default: ./build
  -j, --jobs <count>      Parallel build jobs, default: auto-detect
  --with-tests            Enable NEED_TEST
  --with-benchmark        Enable NEED_BENCHMARK
  --with-debug            Enable NEED_DEBUG
  --with-profile          Enable PROFILE
  --with-debug-info       Enable DEBUG_INFO
  --with-stringent-debug  Enable STRINGENT_DEBUG
  --with-trace            Enable CO_TRACE
  --without-example       Disable NEED_EXAMPLE
  --install               Run shell/install.sh after build (default)
  --no-install            Skip install step
  --cmake-arg <arg>       Append a single extra CMake argument
  -h, --help              Show this help message

Examples:
  ./build.sh
  ./build.sh --type Debug --with-tests --no-install
  ./build.sh --build-dir build/debug -j 8 -- -DCO_TRACE=ON
EOF
}

log() {
    printf '[build.sh] %s\n' "$*"
}

resolve_build_dir() {
    case "$1" in
        /*) printf '%s\n' "$1" ;;
        *) printf '%s/%s\n' "${repo_root}" "$1" ;;
    esac
}

detect_jobs() {
    if [[ -n "${jobs}" ]]; then
        return
    fi

    if command -v nproc >/dev/null 2>&1; then
        jobs="$(nproc)"
        return
    fi

    if command -v getconf >/dev/null 2>&1; then
        jobs="$(getconf _NPROCESSORS_ONLN)"
        return
    fi

    jobs="1"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --type)
            build_type="$2"
            shift 2
            ;;
        --build-dir)
            build_dir="$(resolve_build_dir "$2")"
            shift 2
            ;;
        -j|--jobs)
            jobs="$2"
            shift 2
            ;;
        --with-tests)
            need_test="ON"
            shift
            ;;
        --with-benchmark)
            need_benchmark="ON"
            shift
            ;;
        --with-debug)
            need_debug="ON"
            shift
            ;;
        --with-profile)
            profile="ON"
            shift
            ;;
        --with-debug-info)
            debug_info="ON"
            shift
            ;;
        --with-stringent-debug)
            stringent_debug="ON"
            shift
            ;;
        --with-trace)
            co_trace="ON"
            shift
            ;;
        --without-example)
            need_example="OFF"
            shift
            ;;
        --install)
            need_install="ON"
            shift
            ;;
        --no-install)
            need_install="OFF"
            shift
            ;;
        --cmake-arg)
            cmake_args+=("$2")
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            cmake_args+=("$@")
            break
            ;;
        *)
            printf 'Unknown argument: %s\n\n' "$1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

detect_jobs
mkdir -p "${build_dir}"

log "repo root: ${repo_root}"
log "build dir: ${build_dir}"
log "build type: ${build_type}"
log "jobs: ${jobs}"
log "options: NEED_EXAMPLE=${need_example} NEED_TEST=${need_test} NEED_BENCHMARK=${need_benchmark} NEED_DEBUG=${need_debug} PROFILE=${profile} DEBUG_INFO=${debug_info} STRINGENT_DEBUG=${stringent_debug} CO_TRACE=${co_trace} INSTALL=${need_install}"

cmake \
    -S "${repo_root}" \
    -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE="${build_type}" \
    -DNEED_EXAMPLE="${need_example}" \
    -DNEED_TEST="${need_test}" \
    -DNEED_BENCHMARK="${need_benchmark}" \
    -DNEED_DEBUG="${need_debug}" \
    -DPROFILE="${profile}" \
    -DDEBUG_INFO="${debug_info}" \
    -DSTRINGENT_DEBUG="${stringent_debug}" \
    -DCO_TRACE="${co_trace}" \
    "${cmake_args[@]}"

cmake --build "${build_dir}" --parallel "${jobs}"

if [[ "${need_install}" == "ON" ]]; then
    (cd "${repo_root}/shell" && ./install.sh)
fi
