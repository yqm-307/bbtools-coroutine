---
schema: wfer-execution/v1
plan_id: multi-size-stack-runtime
revision: 2
artifact: design
---
# 多尺寸固定栈端到端支持 · 设计

## 实现方向
### 1. 最小入口及参数链
沿既有 Scheduler 注册和 Coroutine/Context 构造链，不引入 options 对象或新的协程类型。以下签名为拟新增/调整，不声称现已存在：

```cpp
// 默认两种入口保留；宏继续用它们。
void RegistCoroutineTask(const CoroutineCallback& handle, const char* desc = nullptr);
void RegistCoroutineTask(const CoroutineCallback& handle, bool& succ) noexcept;
// 新增，大小在前避免与回调/描述重载混淆。
void RegistCoroutineTask(size_t stack_size, const CoroutineCallback& handle,
                         const char* desc = nullptr);
void RegistCoroutineTask(size_t stack_size, const CoroutineCallback& handle,
                         bool& succ) noexcept;
// detail 链路消除 int 窄化；默认值解析只在入口。
// Coroutine 类内：构造函数同步改 size_t。
static Ptr Create(size_t stack_size, const CoroutineCallback& handle,
                  bool need_protect = true, const char* desc = nullptr);
// StackPool 类内：
ItemType* Apply(); // 默认配置兼容入口
ItemType* Apply(size_t stack_size, bool need_protect);
```
默认注册转调显式重载，读取大小和保护配置；显式大小重载只读保护默认，不改配置。Context 使用 `Apply(stack_size, stack_protect)`；直接构造的显式 bool 真正传入，不再被池的全局保护开关覆盖。已有公开宏/描述入口不动。

Context 的 callable 先构造再借栈；借栈到 make_fcontext 成功期间新增窄作用域 RAII（复用 std::unique_ptr，加自定义归还 deleter），失败必归还，成功才将成员裸指针作为唯一借用责任。Coroutine::Create 暂持 unique_ptr 至描述赋值完成。注册直到既有 `_LoadBlance2Proc` 成功交接前也暂持 unique_ptr；发布后只 release 所有权、不再读取可能立即被 worker 销毁的对象。已核 `Processer.cc:78–85` 的 AddCoroutineTask 入队使用 AssertWithInfo；本点只保证栈申请/构造、描述赋值及无 worker 拒绝等发布前路径，不承诺修复调度队列 OOM 的既有 assert/log 处理。新增参数校验及可恢复错误均须发生在入队之前；不让已发布对象重新落入可抛的清理路径，也不扩改该队列或其他调度策略。

### 2. 一处规范化与 Stack 失败安全
在现有 Stack 类内新增一个供 Stack/StackPool 共用的窄静态规范化函数，返回 U；不建独立 allocator 或策略层。获取/校验 P，拒绝 0；用余数法计算需补的字节，先检查再加，检查 U/保护页总量不超 SIZE_MAX 和 PTRDIFF_MAX，之后才分配或做指针运算。不是假定页大小必为二次幂再位运算。

Stack 新增只读 `IsProtected() const noexcept`；结合既有 UseableSize 形成真实桶键。继续 memalign/Free/mprotect：空返回转 bad_alloc；PROT_NONE 失败保存 errno、释放尚未保护成功的块、清空临时所有权后抛 system_error。不允许日志后继续创建一个声称有保护的栈。凡需在 Release 生效的拒绝一律通过异常/返回值兑现，不由 Assert/AssertWithInfo 承载：这些仓内宏在 NDEBUG 下只日志不终止（`bbt/core/util/Assert.hpp`）；原生 assert 在 NDEBUG 下甚至不求值。释放/入队是实际语句，不能依赖上述断言表达式。

正常释放保持先注销 Valgrind、解除保护、再 free 和归零；Clear 幂等，move 同步转移保护/尺寸/登记。解除保护失败时不从析构抛异常，也不继续 free 可能仍不可写的 malloc 块：记录 errno 后 fail-stop（terminate），仅用隔离子进程验证。该处是不可安全恢复的系统故障，不把它混入普通分配失败恢复策略。

### 3. 分桶目录与同步不变量
使用标准 `std::map<std::pair<size_t, bool>, std::unique_ptr<Bucket>>`，Bucket 直接持有现有 PoolType、空闲/借出/申请中计数；保留正常构建的既有队列和 ValgrindPool，不另造无锁目录。目录、所有桶计数、全池计数、平均值/时间戳由一把池级 mutex 保护，队列只在该协议下访问。

- 以独立精确计数记录 live（池管理的已分配对象）、idle（缓存对象）、borrowed（已借出）、pending（已预留但分配未完成）；稳定时 live=idle+borrowed，申请中容量检查用 borrowed+pending。GetCurCoNum 返回 borrowed+pending，不使用队列近似 Size 推导。
- 内部计数及 AllocSize/GetCurCoNum/GetCoAvgCount 使用 size_t，核对 Profiler 的 to_string 消费和测试探针相应类型；不引入饱和/截断的假统计。PROFILE 分配/释放事件仅在真实对象创建/销毁时累计，复用不记新分配。
- Apply 先规范化；锁内检查全池上限，创建/定位桶并预留。能 Pop 就将 idle 转 borrowed；否则 pending 占容量后解锁，锁外 new Stack/memalign/mprotect。再锁内将 pending 转 borrowed/live；异常回滚 pending，空桶可清理。目录或队列构造失败也必须在没有半份计数修改的路径退出。
- 借出/pending 为桶的寿命 pin；有任一 pin 的桶不得擦除。锁外分配完成/归还按真实键重找，禁止持裸 Bucket 指针越过可能擦除目录的解锁点。
- Release 的前置是该池合法且尚未归还的 Stack；按 Stack 自身 U/保护属性回桶。锁内把 borrowed 转 idle；Push 失败/抛出则撤回缓存、扣 live，并锁外销毁该栈，保证析构路径不抛。不为重复归还/跨池非法指针增加通用所有权追踪表。
- ReleaseUnused 逐个在锁内摘空闲并更新计数、解锁销毁再继续；不为回收额外分配收集 vector，避免 OOM 清理路径再分配失败。不触及借出或申请中栈。OnUpdate 同样只摘空闲，并受本次预先计算的回收数量限制。正常系统锁故障沿 C++ 运行时处理，不以 noexcept 掩饰可能的未处理异常。
- 大块分配/mprotect/free、用户 callable、外部回调以及 Profiler 调用不在池锁内执行；目录/现成队列自身的小型分配可在短临界区，失败路径必须回滚。不同时持池锁调用回调，规避 Context 析构探针等重入锁死。

### 4. 预热及全池回收
`_Preload` 直接为默认桶创建 min(配置预热数,上限) 个独立对象，而不是 Apply→Release 复用循环。分配/入队成功才记 live/idle；构造失败由已持有资源的清理路径排空已预热对象再抛，不把抛异常函数错误标 noexcept。无运行期配置更新支持。

保留全池现有平均采样系数、扫描/调整间隔及最低保留配置。同步保护采样和时间戳；成功采样后更新采样时间。定时回收目标为 max(最低保留数,原有平均目标)，从各桶的空闲对象中删到全池目标或无空闲为止；不依赖每桶平均、不复制最低数。显式 ReleaseUnused 不受最低保留数限制。仅在 idle/borrowed/pending 全零时删目录项，局部池析构要求无借出/在途；全局池仍按 process-lifetime 管理。

## 关键取舍
- 精确 U/保护属性分桶，不取更大桶、不向二次幂归档；最直接兑现用户指定值并让默认 12 KiB 自然共存。
- 一把短锁维护跨桶不变量，而不是多个 atomic 字段假装事务。先保正确性；不增加锁分片、TLS 缓存或性能配置。池级 mutex 处必须写 `ponytail:` 注释：单池元数据锁把各桶申请/归还串行化，多生产线程下存在吞吐天花板；仅在真实快速回归确认该锁成为阻断瓶颈时升级为分桶同步与独立全池容量协议，仍保证跨桶一致性。不提前做该升级。
- 最大值沿既有同时借出约束；缓存总字节不是此次资源预算。多尺寸可能增加驻留内存，文档明确，不偷换成每桶最大值或改成最大总内存。
- 旧小栈参数由忽略变生效；不为了兼容错误行为再次忽略。仅依任务真实需求调整测试/debug 栈请求，保留小值的纯布局测试。
- 不加正式依赖或通用故障框架。参考当前 Coroutine 的一次性失败测试模式，新增窄静态原子测试标志，测试构建才编入；正常生产配置无测试分支/导出。

## 变更面
### 必改源码及说明
- `bbt/coroutine/detail/{Scheduler,Coroutine,StackPool,Stack}.{hpp,cc}` 及 `bbt/coroutine/detail/Context.cc`：上列入口、字节类型、传递、RAII、规范化、属性、分桶、计数、回收/失败清理。`Context.hpp` 原本已是 size_t，仅有实际声明需要调整时按需修改，不列为必改。
- `unit_test/Test_coroutine_stack.cc`：S2/S5–S13 的布局、池、计数、失败/寿命；保留 #279/#379 的释放/移动/回调顺序探针，类型随 size_t 更新。
- `unit_test/Test_scheduler_api.cc`：旧入口/描述/成功输出及新重载、拒绝参数/无 worker（进程分隔）。拟新增 `unit_test/Test_multi_size_stack_runtime.cc`：使用薄 main 的进程模式（run-protected / run-unprotected / reject-before-start），分别注册独立 CTest 进程；正常模式覆盖 S1/S4 真正默认/显式多尺寸挂起恢复，每进程只 Start 一次，无 worker 拒绝模式不 Start。不新增测试框架或依赖。
- `unit_test/CMakeLists.txt`：拟新增 runtime 测试注册和明确超时；该文件仅在根 `CMakeLists.txt` 的 `if(NEED_TEST)` 下被加入，文件内本身没有该分支；就在 `unit_test/CMakeLists.txt` 对已存在的 bbt_coroutine 调用 `target_compile_definitions(bbt_coroutine PUBLIC BBT_COROUTINE_STACK_TESTING)`（拟名，相关 TU 一致），不修改根 CMakeLists.txt。Stack/StackPool 的窄注入点分别覆盖 memalign、建立保护的 mprotect(PROT_NONE)、解除保护的 mprotect(PROT_READ|PROT_WRITE)、缓存 Push 下一次失败。只在测试构建编入全部 seam 声明/定义/分支，不能只定义测试 TU 导致库路径没注入；生产构建必须以源码守卫、编译命令和未 strip 库的正反符号对照证明无注入，不以编译成功替代。
- `README.md`、`agent-docs/api-reference.md`：默认/显式用法、12 KiB 真值、页对齐/保护/非法值、固定栈和重编/CoPool 边界。不修改 CI/发布流程文档来掩盖已知门禁冲突。

### 按直接调用点必要修改
- 对 `unit_test/`、`debug/`、`example/`、`benchmark_test/` 的 Coroutine::Create/Context 构造做实际调用扫描；对原 4096/1024 等重型 fixture 显式给足栈；不能盲目全量替换，也不能删除小尺寸检查。路径以实际扫描结果为准。
- Profiler/OnUpdate、ValgrindPool 消费和试验头独立包含按改动核；不改 CoPool 实现、不动队列公共 API 或其他运行时策略。

## 风险与恢复
- 基线风险：owner 仍是旧 HEAD且有用户 AGENTS 改动；实施在单独工作区基于冻结提交，owner 的 WFER 仍为唯一状态真源。若选择更新基线，先核差异/规则并修订受影响计划，不自动同步代码或 stash/reset 用户文件。
- 注入真实性：四个独立一次性故障标志分别在实际 memalign、建立保护、解除保护和 Push 的结果检查边界制造失败；建立/解除保护是不同调用点，不共用一枚标志。可恢复项验证同一回滚分支，解除保护项验证 fail-stop。后者在 runtime/线程尚未启动的子进程中，对真实已保护的栈置释放侧标志，再 Clear；子进程安装仅用于观测的 terminate handler 以约定哨兵 exit 状态结束，父进程检查 wait 状态，区分命中 terminate、普通返回和无关 SIGSEGV，不把任意子进程失败算通过。正常路径仍执行真实分配、保护、复用和调度。若候选的注入未穿过真实检查分支，不能称验证失败安全；描述/回调赋值异常可用 test-local throwing callable/窄测试探针，不发布新稳定 API。
- 定时与并发：屏障固定峰值容量；等待用单调时间及上限，不靠任意 sleep 排序；定时回收测试允许等既有调整间隔，CTest 设合理 timeout。异常收集在控制线程断言，不能在线程中 throw Boost.Test 后漏 join。保护页 fault 子进程在启动 worker 前 fork，runtime 已启动后的模式用新进程 exec，避免多线程 fork 运行 allocator。
- 生产/诊断：先查 Boost 1.90、ccache、Valgrind/头文件；本机缺 valgrind 不等于测试通过。构建目录不混参数；root CMake 将 unit_test 配为 Debug，Release 验证必须确认库本身 NDEBUG 生效，不能只看测试 target 的标签。
- 性能：独立工作区使用专有 build 目录，快速性能脚本默认读 `build/bin/benchmark_test/unified_stress`，不可拿另一目录旧产物冒充；按既有脚本/基线/指纹运行，缺可比基线明确缺口，不改指纹或写基线。
- 授权：本轮所有实现/测试步骤只是计划；构建入口小改、后续实施/提交/远端动作按 proposal 核权限。普通失败修复重验；改变需求、桶语义或验收则回 draft，并同步 revision 四份。
- 恢复：tasks.md 唯一施工进度，执行一次只勾实际完成步骤；checkpoint 仅存定位/候选/真实运行引用。回退用后续授权工作分支的最小增量，不删用户改动/Map，不以回滚代替失败报告。
