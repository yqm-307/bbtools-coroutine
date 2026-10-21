# Issue #384 剩余诊断验证执行摘要

**Alice 对固定候选 `2c4a672b9b7fa2b2efbcb91957c3d8e6229cf102` 的本次诊断验证验收通过。** 执行者本地 T2、父代理远端核对与独立证据审查已完成。没有代码或测试修改；仅本摘要与 summary.json 随归档提交受 Git 跟踪，原始证据不入仓。此结论不包含报告提交的新 CI、合并、Issue 关闭或发布。

## 候选与环境

- 执行位置：隔离 worktree；分支：`agent/issue384-diagnostics`。下文路径均相对于该 worktree。
- base = HEAD = `2c4a672b9b7fa2b2efbcb91957c3d8e6229cf102`；产品 diff 为空，未操作共享主工作树。
- 源码逐文件 SHA-256：`tests/reports/work/issue384-20261021-native/source-sha256.json`；清单指纹 `b369583406a55684b6fcfee6c7f922fd8c09838a19fe4f741d9a85d1dd0bd15b`，收尾重新校验一致。
- GCC 15.2.0、CMake 4.2.3、Ninja 1.13.2、ccache 4.12.3；12 核，JOBS=3。四档顺序配置/构建，只构建指定目标。
- Boost 1.90.0 context：Ubuntu deb `1.90.0-6ubuntu1`，系统安装前缀，Git SHA 未知；包版本、SHA-256、Build ID 见 dependency-*.json。core/pollevent 内置，不存在外部 core Git SHA。
- ccache、TMPDIR 都在本 worktree 已忽略的 build-384-*。首步写入/读回/清理收据见 probe.json。

## 实际结果

| 配置 | 库实际 TU | 聚焦 CTest | 模式判别 |
|---|---:|---|---|
| Release + NEED_TEST | 37，全含 NDEBUG | 10/10，无 skip | 无 PROFILE/STRICT；真实拒绝与回滚 |
| PROFILE Debug | 37，无 NDEBUG | 10/10，无 skip | 只含 PROFILE；reject-before-start 进程内 CheckProfileStackEvents 实跑 1/1、3/3、3/3 |
| STRINGENT_DEBUG Debug | 37，无 NDEBUG | 10/10，无 skip | 只含 STRICT；Check_IsResumedCo 调用受同宏保护 |
| 生产 Release，NEED_TEST=OFF | 37，全含 NDEBUG | 不构建测试 | 全 TU 无 STACK_TESTING，八 seam 全缺失 |

四档 STACK_DIAGNOSTICS 均 OFF。单测子目录实际强制 Debug，Release 结论只针对库 TU；完整命令见各档 library-commands.json，覆盖集合同时与 Ninja 库链接输入逐一核对。

每档测试前解析 CTest JSON，精确断言 10 项集合相等；8 个可执行文件逐目标 ldd 均命中本档库，LD_LIBRARY_PATH 显式指定。日志保存 BOOST_TEST_LOG_LEVEL=test_suite，58 次 Boost 用例进入记录/档（唯一用例名 56 个）；runtime 三模式另有逐项 Check 输出。没有把整体退出码代替场景检查。

三个测试库八 seam 均存在；未 strip 的生产库定义符号表非空，八 seam 均缺失。四个源文件的声明、定义和注入分支逐行守卫收据见 source-seam-guards.json；完整 nm、库身份与哈希见 *-nm.stdout、*-file.txt、audit.json。

首次 Release 构建步骤输出为 initial-release-build.stdout，后续 no-work 输出不充当构建证据；未采集 ccache 命中统计，不据此区分实际编译与缓存重放。四档构建输出未发现编译器 warning/error。配置均有现有 CMP0167 dev warning，未消除或屏蔽。

## 场景与最终不变项映射

所有下列栈用例均在三档执行；源码位置以固定候选为准，进入记录见 *-ctest.stdout 和 scenario-receipts.json。

| 不变项/判据 | 现有出口 |
|---|---|
| 创建与默认/显式尺寸贯通 | Test_coroutine_stack.cc:116、129；runtime protected/unprotected 的默认、显式、描述宏、succ 重载及配置未反写检查 |
| 页取整与保护布局 | Test_coroutine_stack.cc:180；216 的父进程核验真实 SIGSEGV 与无保护正常退出 |
| 按实际 U/保护属性分桶归还 | Test_coroutine_stack.cc:452；交错借还、配置变化后回原桶、同 U 归一化复用 |
| 跨桶容量、并发借还/空闲回收 | Test_coroutine_stack.cc:547、596、771；屏障固定借出状态，回收不碰借出栈，最终计数归零、定时全池保留目标 |
| 预热独立对象、上限及失败计数 | Test_coroutine_stack.cc:698；runtime RunPreloadFailures 的真实 OOM、发布失败及恢复；PROFILE 对真实销毁计数 |
| 分配/保护/入池失败回滚 | Test_coroutine_stack.cc:837；bad_alloc、EACCES system_error、pending/borrowed 回滚与后续恢复 |
| 解除保护失败 fail-stop | Test_coroutine_stack.cc:334；子进程 terminate sentinel=42，未注入正对照=7，区别无关崩溃 |
| callable 先释放再归还栈 | Test_coroutine_stack.cc:926；析构探针观察 baseline+1，完成后归 baseline；只覆盖 m_user_main，m_onyield_callback 保留态仍是既有边界 |
| Release 拒绝 | runtime reject-before-start：0→invalid_argument，SIZE_MAX/PTRDIFF_MAX→length_error，无分配；无 worker→runtime_error/succ=false，回调未执行、借出计数零；描述赋值真实 bad_alloc 后归还并恢复 |
| 单进程单次 Start | runtime 三模式分别独立 CTest 进程，每个成功运行模式仅一次 Start，reject 模式不 Start；其余复用 once_flag。未把本集合说成重复/并发 Start 专项验收 |
| PROFILE 判别 | Test_profiler 计数/ProfileInfo + runtime CheckProfileStackEvents；后者日志三次命中，不以 Test_profiler 单独通过证明宏生效 |
| STRICT 判别 | Test_stringent_debug.cc:69、89、108 的挂起恢复；132、153 的受管 FINAL；178 登记失败返回 -1 后再挂起；209、226 直接驱动 FINAL；247 关闭登记配对。各 Check_IsResumedCo 宏保护块在严格 Debug 同口径编译并执行，库断言有效 |

## 证据、失败与限制

原始根目录：`tests/reports/work/issue384-20261021-native`。run_matrix.py 为实际执行脚本；每次 configure/build/CTest/ldd 均有 command.json、stdout、stderr、退出码/耗时；CTest 原始 Testing 树也已复制。release-configure 使用独立 .exit 文件。audit.py 为实际宏、输入集合、符号和守卫审计入口。

保留的首轮失败：验证脚本误把二进制写成 bin/Test_smoke，ldd 退出 1；实际产物在 bin/unit_test，修正脚本后所有 ldd 通过。未删失败收据，未改产品、断言或诊断宏，未发生产品测试失败。

`gh issue view 384 --repo yqm-307/bbtools-coroutine --json body,comments,state,title,url` 退出 1：无法连接 api.github.com。执行者因此未核对实时远端记录，原失败保留。父代理已在可访问 GitHub 的环境读回 Issue 全线程及固定 SHA 的 check-run，独立审查也已完成，见下方验收补充。

ASAN、TSan、弱序平台未测；旧 FD 快照/Poller 间歇问题边界保留；历史 Valgrind 只约束原 SHA/工作负载，不能扩成当前混合尺寸/保护模式的 memcheck 通过。未运行本地全量 CTest、长 soak、发布 Gate。

## 执行者交接时的冻结边界

执行者仅新增 archive 下 summary.md、summary.json，交回后停止写入，未提交、未推送、未建 PR、未评论/Review、未关闭 Issue。父代理随后补齐远端核对与最终验收，仅归档这两份摘要；未修改沙箱或全局配置。

## 独立验收补充（Alice 观察与判定）

- 实时读取 [Issue #384](https://github.com/yqm-307/bbtools-coroutine/issues/384) 正文及评论，状态 OPEN，updatedAt=`2026-10-20T14:49:07Z`；远端仍记录旧候选的严格诊断失败，本摘要记录本次固定候选的新验证结果，未回写或覆盖历史记录。
- 父代理读回固定源码 SHA 的 10 项 check-run，均为 completed/success。其中[编译与单元测试](https://github.com/yqm-307/bbtools-coroutine/actions/runs/39578710019/job/119150853077)、[性能回归](https://github.com/yqm-307/bbtools-coroutine/actions/runs/39578710019/job/119150654191)、[文档检查](https://github.com/yqm-307/bbtools-coroutine/actions/runs/39578709165/job/119150650661)分别有对应 job。它们属于固定源码 SHA 的已有 CI，不是本报告归档提交的新 CI；该读回来自父代理环境，不是执行者沙箱联网成功。
- Alice 独立检查四档各 37 个库 TU 的完整源码集合、宏、八 seam、未 strip 身份与产物哈希，全部一致；四档哈希与上表相同。
- Alice 另执行 `ctest --test-dir build-384-release --output-on-failure --verbose -j1 -R '^Test_multi_size_stack_runtime_reject_before_start$'` 与 `ctest --test-dir build-384-strict --output-on-failure --verbose -j1 -R '^Test_stringent_debug$'`，均退出 0。两次复验复用执行者构建树，属于行为复现，未独立重新构建。
- 独立证据审查逐项核对 C1–C8、196 条源码指纹、构建输入与场景断言，结论 PASS；两份摘要由本次归档提交受 Git 跟踪。原始审查和复验日志保存在仓外运行记录，本文保留可长期引用的结论、命令、候选、CI 链接与限制。
- C5 只覆盖登记的八个 Stack/StackPool seam。`Coroutine::_TestFailNextAwaitRegist` 与 `s_test_fail_await_regist` 是 #369 的既有注入点，在生产库仍存在，未纳入本票的八 seam 清单，本次未修改；不声称全库没有测试符号。
- 本次验收通过仅指约定的剩余诊断验证；上文 ASAN/TSan、弱序平台、旧 FD/Poller、历史 Valgrind 外推与发布 Gate 等限制继续保留。远端 Issue 仍 OPEN。
