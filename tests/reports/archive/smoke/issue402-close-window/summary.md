# #402 建立窗口关闭安全：本地聚焦验证

- 基线：`a4d52c230c436fe54d946ded8e94c9ddbb75d85e`；分支：`investigate/issue402-close-window`；事件：GitHub #402（#370 代际守卫的建立窗口补强）。
- R2 完整实测候选 diff SHA256：`036b642fb0fe01bfc447babb396b7e55cb7f176aa2c9cb7ff4e33e2bb0e29369`；`summary.json` 的 `source_files` 保留该轮 6 文件实测输入。R3 最终源码指纹、差异哈希与增量复验见 `final_comment_revision`，不把注释修正后的输入冒充先前完整矩阵输入。
- 产品改动仅 `CoPollEvent.cc` 的 `InitFdEvent` 内 establish-closing 锁存（命中 closing 时不再创建底层 Event，直接 Publish PENDING(CLOSED)），`CoPollEvent.hpp` 改动全为注释行；两份 agent-docs 语义说明同步。公开签名、实例布局（`sizeof=160`、`alignof=16`）、`ProcesserDriver`、根 CMake/CI/依赖均未变（动态符号表与基线完全一致）。

## 验证

Linux x86_64 / GCC 15.2 / Boost 1.90 / C++17；ccache；显式 target 与 max(2,nproc/4)=3 并发；`env -i` 白名单子进程环境；每个执行二进制用 `ldd` 核对到同档 lib 与系统 Boost context。

- 默认 Release：14 个 CTest 用例（冒烟、Hook 主/错误矩阵/超时 flags/blocking fd、epoch 单/多 worker、CoPollevent 状态机、OOM 五模式）**14/14 通过，0 fail，0 skip**；用例级 101 例/模式无失败。
- 严格诊断 Debug + `STRINGENT_DEBUG=ON`：库 TU 37/37 带 `-DBBT_COROUTINE_STRINGENT_DEBUG`；4 个目标（smoke、copollevent_state、hook_fd_epoch、stringent_debug）**4/4 通过**，49 例 0 skip，诊断断言实时生效。
- 基线反例：`git archive` 固定 base + 本轮两份新测试（不复制修复后产品文件）构建出的库上，`t_init_during_close_window_is_closed_not_admitted` 稳定失败（`FdWaiterCount` 1≠0、状态 LISTEN≠TRIGGER、`CommitPark` fatal），5/5 复现；失败点在预期断言，非编译失败/0 例/signal/超时。
- 区分力：隔离快照中删除 `Hook_Read` 的 `wret==2` 分支里 `io.Dismiss()`（单行、仅 read），该用例被 flags 断言杀死（新对象 O_NONBLOCK 丢失），EBADF 与字节断言仍通过。仅此一处，不外推 write 分支或全部 epoch 分支。
- 真实 Hook write sibling（`t_parked_write_close_reuse_returns_ebadf`）：45/45 断言通过；发送缓冲填到 EAGAIN 真 park、控制协程拿到 worker、旧写 -1/EBADF、新 peer 无字节泄漏、新对象保留 O_NONBLOCK、参与者与自有 fd 正常收尾。
- 稳定性抽检：close-window 30/30、write sibling 30/30、readable resume 30/30、并发双关 20/20。
- 编译无新增告警；`git diff --check` 通过；configure 保留既有 CMP0167 开发者告警；验证前后源文件 SHA 与 HEAD/分支均未变。

## 注释收敛后的增量复验（R3）

- 相对 R2 仅校准三份 `.cc` 的注释及摘要说明；用 `c++ -fpreprocessed -dD -E -P -x c++` 对两版去注释后逐字节比较一致，不改变运行语句、断言、签名或布局。
- 最终源码 diff SHA256：`8da3141bd2213d594f8691b49e5c2e60a41bbc812ed9e53b44f8a7faee95c87c`（相对固定基线，仅 6 源码/契约文件，不含两份摘要）。
- 重新构建受影响目标：Release 的 `Test_smoke`、`Test_copollevent_state`、`Test_hook_fd_epoch` **3/3 通过**；Debug 严格诊断加 `Test_stringent_debug`，**4/4 通过**。两档均 0 fail、0 skip、0 编译告警；逐个 `ldd` 核对同档库与 Boost context。
- 未重跑上面的完整矩阵：14/14 与 4/4 是 R2 原始观察，本节 3/3 与 4/4 才是 R3 新运行。原始日志为 `tests/reports/work/issue402/logs/r3-*.log`，JUnit 与库哈希见 `summary.json`。
- R2 完整独立审查结论为 `ship`，无阻断项；本节文案用于处理其 5 条非阻断意见。最终增量审查、远端 CI 和合并分别核验，本记录不预报其结论。

以下 shell 为**可复现模板**，不是实际执行命令原文：实际构建目录为 `build-402-release` / `build-402-strict`，不是示例中的 `build-pr`；真实运行记录在 `tests/reports/work/issue402/logs/`。

```sh
JOBS=$(( $(nproc) / 4 )); [ "$JOBS" -ge 2 ] || JOBS=2
cmake -S . -B build-pr -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEED_TEST=ON \
  -DNEED_BENCHMARK=OFF -DNEED_EXAMPLE=OFF -DPROFILE=OFF -DSTRINGENT_DEBUG=OFF \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-pr --target bbt_coroutine Test_smoke Test_hook Test_hook_contract \
  Test_hook_blocking_fd Test_hook_error_matrix Test_hook_timeout_flags Test_hook_fd_epoch \
  Test_hook_fd_epoch_multi Test_copollevent_state Test_runtime_oom --parallel "$JOBS"
LD_LIBRARY_PATH="$PWD/build-pr/lib" ctest --test-dir build-pr -j1 --timeout 120 --output-on-failure \
  -R '^(Test_smoke|Test_hook|Test_hook_contract|Test_hook_blocking_fd|Test_hook_error_matrix|Test_hook_timeout_flags|Test_hook_fd_epoch|Test_hook_fd_epoch_multi|Test_copollevent_state|Test_runtime_oom_(fresh|prepare|tracked|trigger|defer))$'
```

注意 `Test_hook`/`Test_hook_fd_epoch` 存在前缀同名兄弟用例，regex 必须 `^…$` 锚定；`Test_runtime_oom` 可执行目标不能替代五个 OOM 用例名。

## 边界

- 协议级 close-window 用例直接驱动 `CoPollEvent::BeginFdClose/EndFdClose` 并以 raw `SYS_close` 扮演内层 close，**不是公开 `Hook_Close` 端到端证据**。
- 新增的两个 Hook read/write sibling 用例在**基线库上同样通过**，因此它们是 #370 代际守卫回归，不是 #402 establish-closing 窗口的判别器。
- 未运行本地全量 CTest、ASAN/TSan、弱序平台、Windows/iOS、`poll`/`select` close 时序（本轮不新增也不撤销承诺）、真实客户端长测、多 worker 建立窗口、持续 OOM。R2 首轮 ccache 全 miss；R3 未采集新缓存统计，不声称全量重编或缓存收益。
- 本记录支持本机聚焦测试结论，R2 完整独立审查及 R3 新运行按前文区分；最终增量审查和 PR 当前 head CI 以各自实际记录为准，不据此宣称已合并或可关闭 Issue。
