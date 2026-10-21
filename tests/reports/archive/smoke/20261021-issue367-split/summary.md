# #367 worker stall 快照并发安全：拆分候选验证

- 基线：`41f73016a248ae40b2eeca0131a8688fc68b3b40`；分支：`fix/worker-stall-snapshot-367-ready`。
- 源码清单及 SHA256 见 summary.json；这是从集成候选独立提取的五文件子集，不包含仍有阻断的等待/诊断异常处理。
- worker 与调度线程使用同一 mutex 读写整组快照，移除普通载荷 seqlock 的数据竞争；保留默认关闭、同次停顿去重和独立 busy 信号。
- 回归修正完成收据、失败退出借用寿命、配置初始化时序与 `(co_id, desc)` 真值校验；新增关闭模式独立进程测试。

## 验证

Linux x86_64 / GCC 15.2 / Boost 1.90 / C++17；ccache；显式 target 和 max(2,nproc/4) 并发。每档均显式设置匹配库路径并检查 ldd。

- 默认 Release：冒烟、stall 启用/禁用、scheduler_exec 共4/4；直接耦合的 auto_busy/auto_queue/driver_parked/rebalance_short_backlog、driver api_boundaries/wait_wakeup/query_destruct_release 共7/7；合计11/11。
- PROFILE Debug：冒烟、stall 启用/禁用3/3。
- 编译无新增告警，git diff --check通过；configure仍有既有CMP0167开发者告警。
- 提取的产品与测试文件逐一匹配此前独立审查的已检查清洁子集；整体集成候选的同步/诊断P1不在本PR，不声称整体已通过。

```sh
JOBS=$(( $(nproc) / 4 )); [ "$JOBS" -ge 2 ] || JOBS=2
cmake -S . -B build-pr -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEED_TEST=ON -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build-pr --target Test_smoke Test_worker_stall Test_scheduler_scaling Test_scheduler_exec Test_processer_driver --parallel "$JOBS"
LD_LIBRARY_PATH="$PWD/build-pr/lib" ctest --test-dir build-pr -R '^(Test_smoke|Test_worker_stall|Test_worker_stall_disabled|Test_scheduler_exec|Test_scheduler_scaling_(auto_busy|auto_queue|driver_parked|rebalance_short_backlog)|Test_processer_driver_(api_boundaries|wait_wakeup|query_destruct_release))$' --output-on-failure
```

PROFILE独立配置使用 `-DPROFILE=ON -DCMAKE_BUILD_TYPE=Debug`，只构建/运行 Test_smoke、Test_worker_stall及关闭模式。

## 边界

未运行本地全量CTest、ASAN/TSan、弱序平台或真实客户端长测；全量与性能检查交PR CI。当前仅本地聚焦通过，远端CI及用户review尚待。原始日志不入仓。不修改CI、契约、依赖或Stop/restart。
