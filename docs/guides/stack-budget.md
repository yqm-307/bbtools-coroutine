# 协程入口栈预算与可选诊断

本工具提供**带条件和缺口的编译器模型**，不是 C++ 栈安全证明器，不输出 SAFE。
不改变默认 12 KiB、保护页、栈池、调度、取消或异常契约，不自动修改业务容量。
核心语义仍以 `core-runtime-contract.md`（`docs/architecture/`） 为准。

## 支持边界

- 静态工具：Python stdlib；Linux x86_64、GCC 15.2、SysV ABI、非 LTO。
  真实回归覆盖 `-O0` 和 `-O2`，不把其他优化配置说成已验证。
- 解析器仅支持已实证的 GCC 15.2 **行式 VCG**；新方言/坏格式拒绝，
  不是通用 VCG/C++ parser。保留业务 flags，不自动关 LTO 或降低优化。
- 入口以稳定业务名映射**对象 ID + 精确 linkage symbol + 当前可用 U**。
  不猜 lambda、重载或 demangle；人类签名只用于同一对象内 `.su/.ci` 唯一关联。
  同名 static 按对象区分，最终 ELF 仍不能唯一定位时保持 UNKNOWN。
- 只处理小型、显式登记的对象闭包；weak/COMDAT、别名、cold fragment、
  间接调用、动态栈、递归 SCC、缺少编译元数据、源图/机器边不一致都不能默默闭合。
  本版不提供递归深度语义分析或虚调用候选推断。
- 输入和编译器必须是用户信任的本地构建；本工具不是恶意对象/编译器的沙箱或签名系统。
  从不执行 `compile_commands.json` 的 shell 字符串。
- 不覆盖手工 inline asm/naked 函数等未如实反映在 GCC 栈元数据中的栈操纵；
  不声称能识别任意自定义 ABI、汇编 trampoline 或自定义链接器行为。
  这些路径不能借本工具获得生产上界；普通 SysV/C++ 与摘要中的前提必须实际成立。

## 输出与状态

JSON 使用 `schema_version: 1`，每个入口保留：

| 字段 | 含义 |
| --- | --- |
| `static_known_path_bytes` | 已解析编译帧的路径模型值，不是实际上下界 |
| `conditional_budget_bytes` | 仅当所有缺口闭合时：条件路径 + runtime 摘要 + 显式余量 |
| `recommended_usable_bytes` | 条件预算按显式目标页大小向上取整后的 U，不含保护页 |
| `observed_sample_bytes` | 静态工具固定 `null`；不把容量读回或模型值当运行样本 |
| `unknown_edges` | 缺边、缺元数据、递归、动态栈、ABI/runtime/EH 等未覆盖项 |
| `critical_path` | 条件路径；含 `compiler` / `user_asserted` provenance |
| `static_critical_path` | 与纯编译帧模型对应的路径，可能不同于条件路径 |
| `usable_bytes` | 用户显式登记的当前页对齐 U |
| `assumptions` / `fingerprint` | 模型适用条件及分析输入身份 F |

分支取 `max`，不加总兄弟路径；已内联消失的帧不补回。尾部及跨函数条件跳转
保守相加，并明确这是模型假设。异常/析构/stack-check-failure 路径不能丢弃。
`.su` 的数字本身不能证明 red zone、对齐、返回地址、fcontext、
`Context/Coroutine/std::function` 包装以及 C++ runtime 的完整开销。

| 状态 | 退出码 | 消费容量常量 |
| --- | --- | --- |
| `WITHIN_MODEL` | 0 | 仅精确输入身份与所有声明条件内允许 |
| `EXCEEDS` | 1 | 拒绝；是模型风险，不声称实际必然爆栈 |
| `UNKNOWN` | 2 | 拒绝；未知不是 0，加百分比不能消除 |
| `STALE` | 2 | 拒绝；显式重新分析 |
| `ERROR` | 2 | 拒绝；编译、格式、关联、整数或协议错误 |
| `UNSUPPORTED` | 2 | 拒绝；平台、LTO、新方言等未支持情况 |

该状态/退出码约定适用于 `analyze` / `consume`；`record` 和 `bind-assertion`
成功退出 0 只表示采集/绑定操作完成，不表示已有可用容量模型。

已知路径超过 U 时，即使还有未知项也可报告 EXCEEDS，但条件预算仍为 `null`。
错误/陈旧输入不会生成新容量常量；已有旧头也不能绕过消费校验。
整数按 Linux x86_64 指针差可表示范围检查，余量/页大小不从本机偷偷推断。
U 与建议值额外检查一页保护区的加法，避免生成现有 protected Stack 无法表示的容量。

## 采集与显式清单

先查看 `python3 scripts/stack_budget.py --help` 和各子命令 `--help`。
推荐 `include(scripts/stack_budget.cmake)`，只对选定的、单源文件 OBJECT target 调用：

```cmake
add_library(my_entry OBJECT entry.cc)
target_compile_features(my_entry PRIVATE cxx_std_17)
bbt_stack_budget_collect(my_entry)
```

helper 仅增加本 target 的 `-fstack-usage -fcallgraph-info=su` 和编译 launcher。
保留原单执行文件 launcher（例如 ccache）；不改全局 flags、宏或 IPO。
每个对象旁生成 `.stack-budget.json` 编译凭据。它来自**实际执行的编译 argv**，
包含编译事件、对象/`.su/.ci` 摘要、完整有效依赖文件摘要、宏/driver 摘要、
编译器二进制/版本/目标、flags 和工具版本。
编译前后依赖变化、缺文件、对象替换及不匹配均拒绝。
分析/消费时重新预处理核对有效依赖集合，可发现 include 搜索顺序变化，而非仅看 mtime。
F 也绑定分析脚本内容及 Python 版本；分析和消费须使用同一工具版本/解释器。

也可显式调用 launcher，不传 shell 命令：

```bash
python3 scripts/stack_budget.py record -- /usr/bin/g++ \
  -std=c++17 -O2 -fstack-usage -fcallgraph-info=su -c entry.cc -o build-local/entry.o
```

manifest 示例（路径由本地 CMake 生成，不能照抄为通用路径）：

```json
{
  "schema_version": 1,
  "target": "my_service",
  "objects": [{"id": "entry", "path": "/absolute/build-local/entry.o"}],
  "entries": [{
    "name": "ServiceEntry",
    "object": "entry",
    "symbol": "ServiceEntry",
    "usable_bytes": 65536
  }],
  "page_size": 4096,
  "margin_bytes": 4096,
  "generated_header": "/absolute/build-local/stack_budget_generated.hpp"
}
```

没有 runtime/ABI/EH 覆盖声明时正常结果是 UNKNOWN，不能靠单个具名入口
就宣称完整协程栈已覆盖。没有最终 ELF 和 GNU ld map 也不能生成容量常量。
最终校验要求 map 的实际链接工作目录；CLI 的 `--link-directory` 默认取 map 父目录，
helper 则显式传实际 `CMAKE_BINARY_DIR`，不依赖调用 CLI 时的 cwd。
所有链接输入都必须保留；不要把 source 直接交给链接命令生成随后消失的临时对象。
绑定的人工摘要库必须确为 map 中加载的同一文件，外部符号还须有唯一 strong provider；
IFUNC、weak 或版本选择不闭合时仍 UNKNOWN。静态外部对象还须由最终 ELF 的
strong 符号、实际 map 输入段归属及函数字节共同核实；首版不解析 archive member
的实际抽取选择，人工摘要的外部 provider 来自普通或 thin archive 时保持 UNKNOWN。
动态库则要求是**实际运行装载的同一文件**，不能只凭 map 中同 SONAME 的链接时副本。
工具只对已验证的 `/lib64/ld-linux-x86-64.so.2` 调用 `--list`，不运行 probe/入口；
记录链接时文件与实际运行依赖的独立摘要及 loader 身份。拒绝 LD_PRELOAD/LD_AUDIT。
动态装载器（包括首次 PLT 解析的协程栈开销）也必须纳入人工条件与版本绑定。

## 人工条件摘要

第三方/runtime 无编译元数据时，可以提供 `user_asserted` 条件摘要，
但它**永远不是编译器证明**。模板必须说明来源、依赖版本、目标、配置、假设，
显式列出绑定的实际库文件、外部 linkage 的条件字节数、runtime 额外预算、
每编译帧 ABI 额外预算，并覆盖所有必需类别。缺一类别仍 UNKNOWN。

`bind-assertion --template ... --manifest ... --output ...` 是一次显式批准该模板
适用于这些精确已编译对象的操作；它核对编译器/目标/必需 flags，绑定对象凭据、
模板内容及依赖二进制摘要。`manifest.assertion` 指向该产物。
旧摘要不能跨对象、依赖、版本或配置复用，不能只改一个指纹字符串继续用。

`examples/stack_budget/assertion.json.in` **只是一组未验证的示例作者前提**：
它把 wrapper、fcontext、std::function、EH/析构/回调、stack_chk/abort、
查询函数以及 red-zone/ABI 开销列为独立、显式的条件预算。
这些数值不是测量、上游承诺或生产容量建议，不应直接照搬。
真实应用必须替换为自己审定的版本/配置绑定摘要，否则保留 UNKNOWN。
对间接机器边、cold/别名缺口、动态栈、递归或 weak 选择，不能用这份摘要豁免。
若应用注册过程会发射 `std::function` trampoline，这些**上游包装对象**
必须在分析阶段独立编译、纳入 F，并由 `runtime_symbols` 以对象 ID + 精确 linkage 明确列出。
它们的开销仍属于 `runtime_extra_bytes` 人工前提（包括其间接调用目标只为登记入口），
不是静态用户调用图的自动解析结论；工具核对这些指定函数的最终选择字节及实际输入段归属，
不把未闭合的 weak/COMDAT 选择当已知。该列表不能消除用户入口图中的未知边。

## 两阶段独立消费

独立示例不依赖把本仓 `add_subdirectory` 加入业务工程；先提供已构建的
诊断 OFF runtime 库，Boost 1.90 仍为现有依赖。以下命令从仓库根执行，
所有构建和 TMPDIR 均使用本地忽略目录：

```bash
mkdir -p build-stack-budget-tmp
export TMPDIR="$PWD/build-stack-budget-tmp"
export CCACHE_DIR="$PWD/build-stack-budget-cache"
export CCACHE_TEMPDIR="$TMPDIR"
JOBS=$(( $(nproc) / 4 )); [ "$JOBS" -ge 2 ] || JOBS=2

cmake -S examples/stack_budget -B build-budget-analysis -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DBBT_COROUTINE_LIBRARY="$PWD/build-stack-budget-off/lib/libbbt_coroutine.so"
cmake --build build-budget-analysis --target stack_budget_analysis --parallel "$JOBS"

cmake -S examples/stack_budget -B build-budget-consume -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DBBT_COROUTINE_LIBRARY="$PWD/build-stack-budget-off/lib/libbbt_coroutine.so" \
  -DSTACK_BUDGET_STAGE=CONSUME -DSTACK_BUDGET_ANALYSIS_DIR="$PWD/build-budget-analysis"
cmake --build build-budget-consume --target stack_budget_consume --parallel "$JOBS"
./build-budget-consume/stack_budget_consumer
```

页大小/余量/当前容量分别用 `STACK_BUDGET_PAGE_SIZE`、
`STACK_BUDGET_MARGIN_BYTES`、`STACK_BUDGET_USABLE_BYTES` 显式指定；
示例值不是自动探测的部署事实。容量不足时 analysis target 非零失败。
根工程仅在 `NEED_EXAMPLE=ON` 且额外显式启用
`BBT_COROUTINE_STACK_BUDGET_EXAMPLE=ON` 时注册这一分析示例，默认不接入。

无环协议：

1. ANALYZE 只编译入口/可达对象并链接 probe，分析对象不能直接或间接包含生成头。
   示例将函数指针到 `std::function` 的转换放在 `registration.cc`，
   与 `entry.cc` 一起冻结；不能把 `_M_invoke` 留在会随容量头重编的胶水 TU 中。
2. 只有 WITHIN_MODEL 才生成头；F 包含 manifest、实际编译/依赖身份、
   模型实现、人工条件和最终可达函数选择证据，不包含头或消费胶水。
3. CONSUME 不再声明入口编译 target；直接链接第一阶段的同一非 LTO 对象。
   只有不在协程可达链中的容量胶水 TU 包含生成头。本版限制胶水只发射 `main`，
   任何额外 helper、lambda 或模板 handler 都拒绝，须先移入第一阶段。
4. 消费校验回读真实最终 ELF、GNU ld map、所有链接输入、原对象、头和胶水凭据，
   比较可达函数大小及排除已知 relocation 字段后的实际字节。
   同时按 map 输入段归属核对所选对象，避免把“相同 masked bytes”当 COMDAT 身份。
   不接受用户手填“相同 F”替代这些证据。
5. C 另绑定最终链接对象、库、头和胶水。胶水单独变动只更新 C；
   入口依赖/宏/flags/对象变动（包括相同字节重新编译的编译事件）使旧 F 失效。
6. 每次构建 `stack_budget_consume` 都校验，包括无需 relink 时；
   消费阶段不触发分析，失败要求用户显式回到第一阶段，不循环求固定点。

若曾把生成头放进分析闭包，Ninja 的既有 dep 数据库可能已缓存依赖环；
仅恢复源码不保证旧目录能再次求值。保留该失败目录，用新的分析/消费目录
显式重新采集即可；工具不会替用户删除缓存、清理历史产物或自动迭代解环。

最终链接仍以所声明的标准 GNU ELF 链接与无 interposition 等条件为前提。
非应用的 CRT/库/链接脚本及运行依赖也冻结在 F；未登记、未人工绑定且不只是
`main` 的额外应用对象会使分析 UNKNOWN。消费不能通过另加对象或支持库绕过原闭包。
map、报告和凭据是本地一致性证据，不是对抗恶意篡改的认证体系。
示例通过已冻结的 `StackBudgetRegister(U)` 真正调用已有
`g_scheduler->RegistCoroutineTask(U, &StackBudgetEntry, ...)`，
并在协程内读回 `GetLocalCoroutineStackSize()` 比较页对齐建议 U。
该读回是**总容量**，不是已用栈字节或 B4 运行样本。

## 可选运行诊断

根 CMake 的 `BBT_COROUTINE_STACK_DIAGNOSTICS=ON` 默认关闭，独立于 PROFILE。
ON 仅支持 Linux x86_64 + GNU，库内部及用户选定的业务 target 使用
`-finstrument-functions`；用户 target 需自己显式加该选项。
hook TU 固定 `-fno-instrument-functions` / `no_instrument_function`，
用真实 `%rsp` 采样，无分配、重日志、锁、异常、取消或全栈填色。
TLS 仅保存当前 Context 指针，历史保存在 Context；让出前清空，新 owner
恢复前重新挂接。诊断库须启动期链接，不支持晚期 dlopen 的 initial-exec TLS 用法。

宏 ON 时 `Coroutine/Context::GetStackBudgetSample()` 返回
`observed_sample_bytes`、`sample_count`、`coverage=PARTIAL`：
仅允许当前唯一 owner 在同步安全点调用，或已同步确认不运行的调用方读取。
无同步并发读取不受支持；异步信号处理器不得调用快照/归属 API。
OFF 不增加对象成员、诊断分配、hook 定义/引用或栈写入。

样本包含 hook 自身开销及插桩改变后的布局，不能外推生产峰值；
mid-function alloca、未插桩第三方和信号路径不保证覆盖。
大帧可能在入口 hook 前已经预留，绝不承诺提前拦截爆栈。
静态报告不会把它合并为预算证明；应用若导出样本，须另外记录其诊断构建身份。

## 聚焦验证

`python3 scripts/test_stack_budget.py` 使用真实 GCC O0/O2 产物，
只在忽略的 `build-stack-budget-python/` 或显式测试构建目录保存原始证据。
在支持的编译器/平台上 CTest 注册为 `Test_stack_budget_static`，
其他组合显式提示 UNSUPPORTED；ON 另有 `Test_stack_budget_runtime`。
本地只运行冒烟、本功能与直接耦合目标，不运行全量 CTest。
原始 `.o/.su/.ci`、ELF、stats、日志和采样不入 Git。
