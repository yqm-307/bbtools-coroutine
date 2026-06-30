#!/bin/bash
# build_sanitizer.sh — ASAN + UBSan 构建与运行
#
# 用法:
#   bash scripts/build_sanitizer.sh build        # 构建 ASAN+UBSAN 版
#   bash scripts/build_sanitizer.sh run [dur]    # 运行全模块疲劳测试（默认 600s=10min/模块）
#   bash scripts/build_sanitizer.sh run-module <module> [dur]  # 运行单模块
#   bash scripts/build_sanitizer.sh clean        # 清理 build_san/
#
# 契约参考: docs/testing-contract-spec.md §4.5
# ADR:       docs/adr/0001-testing-contract.md D4 (TSan 不纳入 CI)

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build_san"
BINARY="$BUILD_DIR/bin/benchmark_test/unified_stress"

MODULES=(comutex corwmutex cocond chan copool coroutine)

# ASAN 配置: detect_leaks=0 因为协程栈分配会产生假阳性; halt_on_error=0 收集所有错误
export ASAN_OPTIONS="detect_leaks=0:halt_on_error=0:log_path=/tmp/asan_report"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0"

build() {
    echo "[build_sanitizer] Building ASAN+UBSAN in $BUILD_DIR ..."
    rm -rf "$BUILD_DIR" && mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake "$PROJECT_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DNEED_TEST=ON \
        -DNEED_BENCHMARK=ON \
        -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
    ninja -j$(nproc)

    echo "[build_sanitizer] Done. Binary: $BINARY"
    if [ -x "$BINARY" ]; then
        echo "[build_sanitizer] Sanitizer symbols:"
        nm -C "$BINARY" | grep -c '__asan\|__ubsan' || true
    fi
}

run_module() {
    local module="$1"
    local dur="${2:-600}"  # default 10min per module

    if [ ! -x "$BINARY" ]; then
        echo "[build_sanitizer] ERROR: Binary not found. Run 'build' first."
        exit 1
    fi

    local log_file="/tmp/sanitizer_${module}_$(date +%Y%m%d_%H%M%S).log"
    echo "[build_sanitizer] Running module=$module dur=${dur}s log=$log_file"

    # 用 timeout + SIGKILL 确保不卡在 shutdown
    timeout -s KILL $((dur + 30)) \
        FATIGUE_INTERVAL=60 \
        "$BINARY" --module="$module" --threads=2 "$dur" 0 0 \
        > "$log_file" 2>&1 || true

    local exit_code=$?
    echo "[build_sanitizer] Module=$module exit_code=$exit_code"

    # 检查 sanitizer 报告
    if ls /tmp/asan_report.* 2>/dev/null | head -1 | grep -q .; then
        echo "[build_sanitizer] ⚠️  ASAN reports found!"
        tail -5 /tmp/asan_report.* 2>/dev/null || true
    else
        echo "[build_sanitizer] ✅ No ASAN reports"
    fi

    # 检查 stderr 是否有 sanitizer 输出
    if grep -q 'SUMMARY: AddressSanitizer\|runtime error:' "$log_file" 2>/dev/null; then
        echo "[build_sanitizer] ⚠️  Sanitizer errors detected in log!"
        grep -A3 'SUMMARY: AddressSanitizer\|runtime error:' "$log_file" | head -20
    fi

    # 输出最后一条 FATIGUE_METRIC
    local last_metric=$(grep 'FATIGUE_METRIC:' "$log_file" | tail -1)
    if [ -n "$last_metric" ]; then
        echo "[build_sanitizer] Last metric: ${last_metric:0:200}..."
    fi

    echo "[build_sanitizer] Module=$module complete"
}

run_all() {
    local dur="${1:-600}"
    echo "[build_sanitizer] Running all ${#MODULES[@]} modules, dur=${dur}s each"

    local report_dir="$PROJECT_DIR/tests/reports/sanitizer_$(date +%Y%m%d_%H%M%S)"
    mkdir -p "$report_dir"

    for m in "${MODULES[@]}"; do
        echo ""
        echo "=== Module: $m ==="
        run_module "$m" "$dur"
    done

    echo ""
    echo "[build_sanitizer] All modules complete."
    echo "[build_sanitizer] Check /tmp/asan_report.* for ASAN findings"

    # 汇总
    local total_asan=$(ls /tmp/asan_report.* 2>/dev/null | wc -l)
    if [ "$total_asan" -gt 0 ]; then
        echo "[build_sanitizer] ❌ FAIL: $total_asan ASAN report(s) found"
        exit 1
    else
        echo "[build_sanitizer] ✅ PASS: No ASAN/UBSan errors"
    fi
}

clean() {
    echo "[build_sanitizer] Cleaning $BUILD_DIR ..."
    rm -rf "$BUILD_DIR"
    rm -f /tmp/asan_report.*
    echo "[build_sanitizer] Cleaned"
}

case "${1:-}" in
    build)   build ;;
    run)     run_all "${2:-600}" ;;
    run-module) run_module "${2:?module required}" "${3:-600}" ;;
    clean)   clean ;;
    *)
        echo "Usage: $0 {build|run [dur]|run-module <module> [dur]|clean}"
        echo ""
        echo "Examples:"
        echo "  $0 build              # Build ASAN+UBSAN binary"
        echo "  $0 run 600            # Run all 6 modules, 10min each"
        echo "  $0 run-module comutex 600  # Run comutex only, 10min"
        echo "  $0 clean              # Remove build_san/"
        exit 1 ;;
esac
