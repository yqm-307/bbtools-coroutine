---
schema: wfer-execution/v1
plan_id: multi-size-stack-runtime
revision: 2
artifact: tasks
---
# 多尺寸固定栈端到端支持 · 施工表

已进入本地 apply；当前进度以实际勾选及接续记录为准。默认依章节执行；测试可先于实现锁行为，不能越过权限/基线前提。每步做完才勾，失败或等待不勾；所有施工进度只维护在此文件。

## 1. 实施前置与失败基线
- [x] 1-1 核新的 apply/deliver 实施指令和 `unit_test/CMakeLists.txt` 的新 target/诊断定义精确授权（仅该文件，不修改根 CMakeLists.txt）。基于 `b172f53e66ee8c7e5008272d3a4eaccc23206647` 准备隔离工作区；不切换/覆盖 owner 的旧 HEAD 与 AGENTS.md 改动，不复制 WFER 真源。读取实施工作区 AGENTS/核心契约，变化影响方案则先对账。
- [x] 1-2 检查 Boost 1.90/context、编译器、CMake、ccache、Valgrind/头文件及构建目录来源；记录候选 SHA+未提交 diff 哈希、配置和实际工具版本。核本地局部/CI 全量分工；不安装系统依赖、不改 CI 来绕过缺失，必要依赖缺失就保留阻塞。
  - 实测：隔离 worktree/基线与仓规已核，CMake 找到 Boost 1.90/context，GCC 15.2、CMake 4.2.3、ccache 4.12.3、页 4096；基线两 target 构建及 2 个聚焦 CTest 通过。无新增编译器警告；CMP0167 是基线 configure 的既有 dev warning。Valgrind/头文件未找到，必需诊断仍缺证据，不因此勾选 6-2。
- [x] 1-3 在实际候选扫描 `Coroutine::Create`、`Coroutine(`、`Context(`、StackPool/Profiler 消费及大小窄化，区分定义/调用，逐个记录旧 4096/1024 等 fixture 的真实任务栈需求；检查 `unit_test/`、`debug/`、`example/`、`benchmark_test/`。确认 Scheduler→Processer 发布前后所有权边界，不把队列 OOM assert/log 说成本点已修。
- [x] 1-4 在 `unit_test/Test_coroutine_stack.cc` 先补最小失败断言：Context 的显式大小/保护贯通、多桶精确复用、跨桶容量屏障；新 API 未存在时记录编译失败/旧路径反例与候选，不捏造 RED 结果。
  - 实测：Context显式大小原始RED（12288≠69632、12288≠32768，CTest exit8）保留于build-debug-stack/red-test.log；保护、精确复用、容量断言已补并在Debug/Release执行。本轮PROFILE预热销毁计数新增真实RED（1/0、3/0、3/0）后补共享_FreeItem转绿（1/1、3/3、3/3），证据msr-revision2.R37JhP内对应raw。

## 2. Stack 的共同规范化与失败安全
- [x] 2-1 修改 `bbt/coroutine/detail/Stack.hpp/.cc`：共用页取整函数与只读保护属性；拒绝 0、对齐/保护加法及 PTRDIFF_MAX 越界，保留 memalign+mprotect，错误在 Release 也可观察。对照 S2/S9；不引入二次幂档位或新配置。
- [x] 2-2 在 `Stack.cc` 明确 memalign 空返回和建立保护失败的异常/资源清理，保留 move/Clear/Valgrind 顺序；解除保护失败采用隔离可验证的 fail-stop，不在析构抛错、不 free 不可写块。补 `Test_coroutine_stack.cc` 的布局/保护 fault 子进程和真实移动/幂等断言（S2/S3/S13）。

## 3. 分桶、容量与回收
- [x] 3-1 修改 `StackPool.hpp/.cc`：按 `(U, protect)` 标准 map 目录，Bucket 复用现有 PoolType；池锁保护目录及精确 idle/borrowed/pending/live 计数，size_t 消除窄化。保留无参 Apply 转调，新增显式 Apply；lock 外分配，失败回滚，桶 pin 禁止在途擦除（S5–S10）。池级 mutex 处写 `ponytail:` 注释：元数据串行化的多线程吞吐天花板，及真实回归证明瓶颈后才升级分桶同步＋全池容量协议。
- [x] 3-2 在 `StackPool.cc` 实现按对象属性 Release，缓存 Push 失败则移出管理并锁外 delete，不靠 assert 完成动作；不新增重复归还/跨池指针追踪器。ReleaseUnused/析构跨桶仅清空闲，逐个摘除避免清理时动态收集分配（S6/S8/S10/S12）。
- [x] 3-3 修 `_Preload` 真正分配独立默认栈、部分失败清理；OnUpdate 保留全池平均系数/间隔/最低数、修正采样时间更新，目标 max(最低数,平均目标)，无每桶倍增。Profiler 事件只算真实新建/销毁，读取不靠 size_approx；更新必要消费类型及 probe 类型（S7/S11/S12）。

## 4. 参数链、兼容调用与文档
- [x] 4-1 修改 `Scheduler.hpp/.cc` 增加 size_t 在前的两种注册重载，默认入口转调，描述与成功输出契约保持；修改 `Coroutine.hpp/.cc` 构造/Create 字节类型及描述赋值 RAII，`Context.cc` 传大小/保护并使用窄作用域栈归还保护。保留 callable 先释放再归还，成功发布后不触碰对象，不修改 Processer 调度策略（S1/S4/S9/S10/S13/S14）。
- [x] 4-2 按 1-3 实际清单修改需要足够运行栈的旧直接调用，保留 1/P 等纯布局边界测试；在 `Test_scheduler_api.cc` 编译并运行默认、描述、成功输出、新重载与非法值路径。对默认宏/描述宏保持原语法，相关公开头做 C++17 独立包含与真实调用编译（S14）。
- [x] 4-3 更新 `README.md`、`agent-docs/api-reference.md` 的默认 12 KiB、字节/页对齐、显式调用、失败行为、固定栈与 detail ABI 重编、CoPool worker 栈边界；示例不混 C++20，不说任意极小栈可执行业务，不改发布流程以消除已知规则差异（S14）。

## 5. 验收用例与故障可观测性
- [x] 5-1 新建 `unit_test/Test_multi_size_stack_runtime.cc` 的薄 main，模式 `run-protected`、`run-unprotected`、`reject-before-start`；在 `unit_test/CMakeLists.txt` 注册 `Test_multi_size_stack_runtime_protected`、`Test_multi_size_stack_runtime_unprotected`、`Test_multi_size_stack_runtime_reject_before_start`，共用 target `Test_multi_size_stack_runtime`，明确 TIMEOUT。正常模式启动前设非默认值，在同一 runtime 中交错默认/显式且足够大小，真实有界挂起/唤醒并检查前后大小/局部数据；拒绝模式不 Start（S1/S4/S10）。
- [x] 5-2 在 `Test_coroutine_stack.cc` 补 S5/S6 的单缓存确定性地址复用、两种请求归一化同桶、不同尺寸/保护隔离；S7 的屏障峰值容量（成功者持有到第二屏障）；S8 的受控混合借还/回收、无重复借出和最终计数。工作线程只采样，不在其中做可能抛出的 Boost.Test 断言；有截止时间且必 join。
- [x] 5-3 经 1-1 的构建入口权限，仅在 `unit_test/CMakeLists.txt`（由根 `if(NEED_TEST)` 加入、文件内无 NEED_TEST 分支）调用 `target_compile_definitions(bbt_coroutine PUBLIC BBT_COROUTINE_STACK_TESTING)` 向库与消费 TU 传播同一诊断定义；不改根 CMakeLists.txt。Stack/StackPool 用该宏守卫全部 seam 声明/定义/分支，四个独立一次性标志分别为 memalign、建立保护 PROT_NONE、解除保护 PROT_READ|PROT_WRITE、缓存 Push 失败，穿过真实检查分支。memalign、建立保护、缓存 Push 失败以及构造/描述失败和无 worker 清理属可恢复路径，核计数回滚、回调未发、后续成功；解除保护失败只在 5-4 隔离子进程核 fail-stop，不要求其继续申请。生产无注入的结构与符号证据在 6-2 核验（S9/S10/S13）。
- [x] 5-4 补 S11 的多个唯一预热栈、超上限裁剪与部分失败清理；S12 的持有栈跨回收与最终清零，且通过 OnUpdate 的真实间隔/样本验证多桶全池保留目标；S13 保留既有 #279/#379 探针，加入解除保护失败子进程：runtime/其他线程未启动时创建真实保护栈，置 5-3 的释放侧标志再 Clear；用子进程 terminate handler 的约定哨兵 exit 状态与父进程 wait 检查证明命中 fail-stop，普通返回/无关崩溃不能通过。测试改变全局配置只在 runtime/其他线程不存在时并还原；不得在已启动进程用 Stop/restart 重置（S11–S13）。

## 6. 真实本地验证与适用诊断
- [x] 6-1 在专有实施 worktree，分别 Debug/Release 配置并使用 ccache、保守并发，仅构建下列目标和 1-3 新确认的直接目标；对新编译警告判定及修复。先用 `ctest -N` 核选择实际含用例，再运行本功能/直接耦合和 smoke，不能本地跑全量（S1–S15）。
  - 已核：历史r3 Debug烟测7/8失败保留。deliver最小修Test_copool.cc等待worker后处理及callback结果先写后发布；父补steady_clock实际deadline，最终候选两配置增量build无warning、CoPool1/1、smoke8/8、focus13/13全部rc0，ctest-N集合逐项对账。真实命令/raw与19文件manifest见msr-deliver-final.z3vy_vkv；非刷绿或既有运行时缺陷全免。
- [ ] 6-2 按分离构建配置运行 PROFILE=ON、STRINGENT_DEBUG=ON 的相关库/功能编译及测试；额外 NEED_TEST=OFF 的独立生产配置编译 `bbt_coroutine`，核源码全部 seam/分支受 `#ifdef BBT_COROUTINE_STACK_TESTING` 守卫、库实际编译命令不含该宏，并对未 strip 的实际库执行 `nm -C --defined-only`：先验证测试开启产物存在每个具名 seam，再对生产产物确认对应符号全缺失且符号表非空；不以编译成功或已 strip 后空表当证据。Release 检查库编译命令含 NDEBUG，并以非法值/失败用例的实际异常或返回值验证拒绝，不以日志或表达式求值代替；测试本身由 root CMake 子目录 Debug 不可冒充 Release 库证明。Valgrind 在可用时 NEED_VALGRIND=ON 做聚焦泄漏/栈登记验证；必需诊断缺环境不得打勾通过（S13/S15）。
  - 接续：PROFILE7/7、NDEBUG库拒绝/释放与生产无新注入结构/非空符号表已实核；Valgrind工具/头缺，STRINGENT_DEBUG基线shared_from_this编译失败。环境就绪与既有诊断缺陷处理后续验，不以缺工具当通过。
- [x] 6-3 仅按现有 `scripts/ci_perf_check.py` 快速回归口径运行（默认 build 路径必须对应本候选 Release 库和 unified_stress）。固定主机/参数/基线指纹；无基线、WARN、未测分别报告，不写基线或伪造指纹，不跑长 soak/真实客户端（S16）。
  - 已核：历史本地NO_COMPARABLE_BASELINE保留。用户批准同runner base/head自动比较后，PR最终head与合并main六模块各45s/2线程真实比较全部PASS，命令和库绑定在CI日志可核；见下方当前归档记录。正式基线未改。

### 6-A 已核入口的命令形状（实施时运行，本轮未运行）
在已确认的独立 worktree 根目录；Debug 使用 `build-debug-stack`，Release 使用 `build` 以匹配既有性能脚本。不复用其他候选的目录，实际 CMake 依赖/平台参数在 1-2 核清，不照抄不存在路径。

```bash
cmake -S . -B build-debug-stack -DCMAKE_BUILD_TYPE=Debug   -DNEED_TEST=ON -DNEED_BENCHMARK=OFF -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   -DNEED_TEST=ON -DNEED_BENCHMARK=ON -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
# 对每个 B=build-debug-stack / build 执行；新增目标仅在实现/注册后存在。
cmake --build "$B" -j 2 --target Test_coroutine_stack Test_multi_size_stack_runtime   Test_scheduler_api Test_coroutine_api Test_scheduler_exec Test_worker_tls_boundary   Test_coroutine Test_coroutine_exception Test_poller Test_chan Test_comutex   Test_coevent Test_copool Test_smoke
ctest --test-dir "$B" -N -R '^(Test_coroutine_stack|Test_multi_size_stack_runtime_.*|Test_scheduler_api|Test_coroutine_api|Test_scheduler_exec|Test_worker_tls_boundary)$'
ctest --test-dir "$B" --output-on-failure -R '^(Test_coroutine_stack|Test_multi_size_stack_runtime_.*|Test_scheduler_api|Test_coroutine_api|Test_scheduler_exec|Test_worker_tls_boundary)$'
ctest --test-dir "$B" -N -L smoke
ctest --test-dir "$B" --output-on-failure -L smoke
# 性能：目标名在实施基线 benchmark_test/CMakeLists.txt 再核后增量构建。
cmake --build build -j 2 --target unified_stress
python3 scripts/ci_perf_check.py --module=coroutine --threads=1 --dur=45
```

诊断构建用独立目录追加现有 `-DPROFILE=ON`、`-DSTRINGENT_DEBUG=ON` 或 `-DNEED_VALGRIND=ON`，不是修改 CI。Valgrind 命令在读取该实施基线现有内存检查入口后执行（含合适的 error-exitcode 与清空池路径），不得假定仅有编译开关就已跑 memcheck。

## 7. 独立审查、CI 与交付
- [x] 7-1 自审并固定最终候选：相对基线完整 diff/未跟踪文件、所有 S 场景映射及真实证据、未覆盖边界。交未参与实现者审查入口兼容、整数/所有权、并发 pin/容量/回收、Release/诊断和测试有效性；阻断修复后只复审相关增量及同根残留。计划审查不能替代实现审查（S16）。
- [x] 7-2 核实际提交/远端/CI 授权及仓规；获准后按现有 issue/PR 模板交接稳定 Map/waypoint 身份与证据，独立查询精确候选 required checks 和 CI 全量 CTest。未获准或 CI pending/失败保持未勾选并写接续，不能靠本地局部测试标路径点 done（S16）。
  - 已核：按用户授权提交/推送、Issue384/PR385、独立增量审查、精确required checks成功并合并；新main全量CTest57/57及性能/文档均success。见下方当前归档记录。
- [ ] 7-3 对照下表和不变项验收最终候选；确认没有未核实在途、没有用户改动被覆盖，记录完整范围/证据/缺口。实施出口达标才将 proposal 置 done；PR 就绪仍不合并/发布，Map 归档另需确认。
  - 留缺口：代码已合并且main CI验收通过，6-3/7-2闭合；6-2诊断尚未齐，完整实施出口未满足，proposal保持blocked、不标done。用户已明确接受incomplete归档，非诊断通过；无发布。

## 最终验收
场景 → 主要施工及验证入口（表仅映射，不复制进度）：

| 场景 | 实现/测试步骤 | 证据入口 |
|---|---|---|
| S1/S4 | 4-1、5-1、6-1 | runtime 两种保护模式及每进程一次启动 |
| S2/S3 | 2-1、2-2、6-1 | 布局边界＋保护页子进程 |
| S5/S6 | 3-1、3-2、5-2 | 确定复用及属性隔离 |
| S7/S8 | 3-1、3-3、5-2 | 屏障峰值＋并发借还/回收 |
| S9/S10 | 2-1、4-1、5-3、6-1 | 非法值、注入、构造/无 worker 失败回滚 |
| S11/S12 | 3-2、3-3、5-4 | 真实预热、定时/显式空闲回收 |
| S13 | 2-2、4-1、5-4、6-2 | 移动/Clear/回调顺序、故障子进程、Valgrind |
| S14 | 1-3、4-2、4-3、6-1 | 旧调用扫描、兼容编译/运行、文档对应 |
| S15 | 6-1、6-2 | Debug/Release/诊断/生产配置真实日志 |
| S16 | 6-3、7-1～7-3 | 最终候选独立审查、性能结果、CI/出口 |

每个场景均需满足，而非表格写有映射就算通过。必需检查未知/缺失不能用“跳过”刷绿；若用户另改交付范围，先修订四件套及 revision。程序算出的 task 总数仅结构统计，不是业务完成率。

## 接续说明
### 合并后交付及留缺口归档（当前事实，优先于下方历史）
用户已批准同runner base/head性能CI简化修复，并明确授权合并；PR #385 已合并到 main `b36f5c4cd4406162c093f4e886ee5cab3915e9c8`。PR最终head `b60860a22c559a9eed5cb12241fb089f25832509` 与merge树一致；main push CI `38225087248`、Docs Check `38225086317` 均success。main CTest首轮57/57、smoke14/reliability23 cases无错误；六模块各45s、2线程base/head真实比较全部PASS，未改正式基线/required/仓库变量。独立实现审查链及7文件CI增量审查GO已完成。

稳定证据：
- https://github.com/yqm-307/bbtools-coroutine/pull/385
- https://github.com/yqm-307/bbtools-coroutine/actions/runs/38225087248
- https://github.com/yqm-307/bbtools-coroutine/actions/runs/38225086317
- https://github.com/yqm-307/bbtools-coroutine/pull/385#issuecomment-6120962390

用户在看到缺口后明确「没事留缺口归档吧」。本次归档是incomplete，不撤销S13/S15诊断要求，不把6-2或完整7-3标为通过；不重开开发/压测、不安装工具、不发布版本。残留：STRINGENT_DEBUG基线shared_from_this编译失败，Valgrind工具/头缺未测；PR阶段Test_hook_fd_epoch首轮FD快照15→14、重试后通过，main首轮通过不代表根因已修；历史Poller首发abort未定位。无自有子代理/构建或CI在途，用户原AGENTS修改保留。以后需要补诊断时必须明确恢复本图并核对应候选与环境。
