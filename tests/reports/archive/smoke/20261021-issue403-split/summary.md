# #403 timer 构造回滚：拆分候选验证

- 基线：`41f73016a248ae40b2eeca0131a8688fc68b3b40`；分支：`fix/timer-constructor-rollback-403-ready`。
- 源码清单及 SHA256 见 summary.json；独立四文件子集，不包含同步/严格诊断的未完成修复。
- stream_descriptor adopt 后，timer 创建失败先 release descriptor 所有权再 close 私有dup fd，防止析构二次关闭；与CancelListen单一收口保持一致。
- 测试通过真实timer分配故障与close符号插入，验证关闭次数恰为1、复用同号fd存活、用户fd及callback登记无残留；正常构造为正对照。

## 验证

Linux x86_64 / GCC 15.2 / Boost 1.90 / C++17；ccache；显式target和 max(2,nproc/4) 并发。显式匹配LD_LIBRARY_PATH并ldd核对。

- 默认Release库：smoke、timer失败/正常两模式、eventloop_backend、eventloop_contract、copollevent_state、coevent，共7/7。
- 编译无新增告警，git diff --check通过；configure保留既有CMP0167开发者告警。
- 产品与新回归逐项匹配已由独立审查核查清洁的#403子集；既有基线反例曾得到closes=2，修复后为closes=1且同号复用对象存活。

```sh
JOBS=$(( $(nproc) / 4 )); [ "$JOBS" -ge 2 ] || JOBS=2
cmake -S . -B build-pr -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEED_TEST=ON -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build-pr --target Test_smoke Test_event_constructor_rollback Test_eventloop_backend Test_eventloop_contract Test_copollevent_state Test_coevent --parallel "$JOBS"
LD_LIBRARY_PATH="$PWD/build-pr/lib" ctest --test-dir build-pr -R '^(Test_smoke|Test_event_constructor_rollback_(rollback_timer_fail|construct_ok)|Test_eventloop_backend|Test_eventloop_contract|Test_copollevent_state|Test_coevent)$' --output-on-failure
```

## 边界

故障注入采用本次Linux/GCC/libstdc++环境，不承诺任意OOM恢复。未运行本地全量CTest、ASAN/TSan、弱序平台或真实客户端长测；完整验证交PR CI。当前远端CI和用户review未完成。原始日志不入仓，不改CI、契约、依赖或Hook并发范围。
