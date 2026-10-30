#!/bin/bash
# 手动对比测试：libgo / go / bbtco。
# 需先在仓根构建产出 libbbt_coroutine.so（默认取 ${repo_root}/build，
# 可用 BBT_BUILD_DIR 覆盖）。libgo/go 工具链缺失时对应目标自动跳过。

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "${script_dir}/../.." && pwd)
build_dir="${BBT_BUILD_DIR:-${repo_root}/build}"

if [ ! -d "${script_dir}/bin" ]; then
    mkdir "${script_dir}/bin"
fi

# Issue #12 P2 起 bbt/core 依赖闭包与 bbt/pollevent 已内置进
# libbbt_coroutine，头文件与 CountDownLatch 符号均由本仓提供。
# libgo_test 是对照组：只缺 CountDownLatch/Mutex 两个符号，直接编译
# 对应 .cc，不链接 libbbt_coroutine——避免 Hook.cc 导出的 syscall
# hook 符号经 ELF interposition 互插进对照组进程、污染性能数字。
g++ -std=c++17 -I"${repo_root}" -o "${script_dir}/bin/libgo_test" \
    "${script_dir}/100w_task_libgo.cc" \
    "${repo_root}/bbt/core/thread/lock/CountDownLatch.cc" \
    "${repo_root}/bbt/core/thread/lock/Mutex.cc" \
    -llibgo -ldl -lpthread
g++ -std=c++17 -I"${repo_root}" -o "${script_dir}/bin/bbtco_test" \
    "${script_dir}/100w_task_bbtco.cc" \
    -L"${build_dir}/lib" -lbbt_coroutine -lpthread -ldl

sleep 1
if [ -f "${script_dir}/bin/libgo_test" ]; then
    echo "===== libgo_test begin ====="
    time "${script_dir}/bin/libgo_test"
fi


sleep 1
if [ -f "${script_dir}/bin/bbtco_test" ]; then
    echo "===== bbtco_test begin ====="
    time "${script_dir}/bin/bbtco_test"
fi


sleep 1
echo "===== go test begin ====="
time go run "${script_dir}/100w_task_go.go"
