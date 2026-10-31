# bbtools-coroutine

基于 boost.context 实现的有栈 C++ 协程库，Go 风格语法，仅支持 Linux 平台。

特点：

1. 有栈协程，支持协程间的高效切换
2. 丰富的协程同步原语（Chan、CoSelect、CoMutex、CoRWMutex、CoCond、CoPool 等）
3. 支持协程池，方便管理和复用协程
4. 类似 Go 语言的协程语法和使用体验
5. 事件驱动 I/O，Poller 后端可替换
6. syscall hook：协程内 socket / sleep / poll / DNS 等阻塞调用转为协程等待，不阻塞调度线程；非协程上下文直通原生
7. 按已获得服务量排序的 P 本地 CFS 队列，保留跨 worker 迁移账目
8. 支持协程间的 defer 语义和异常处理

## 文档地图

| 文档 | 内容 |
|------|------|
| [docs/guides/user-guide.md](docs/guides/user-guide.md) | 使用手册：基础用法、综合示例、注意事项 |
| [docs/guides/api-reference.md](docs/guides/api-reference.md) | API 参考：签名、返回码、挂起条件与生命周期 |
| [docs/architecture/core-runtime-contract.md](docs/architecture/core-runtime-contract.md) | 核心运行时契约（唯一契约真源）：进程寿命、线程模型、Hook 边界、driver 分层 |
| [docs/guides/development.md](docs/guides/development.md) | 开发与发布流程：验证阶梯、本地入口、发布 Gate |
| [docs/guides/performance.md](docs/guides/performance.md) | 性能问题分析与优化流程、测量协议 |
| [docs/guides/stack-budget.md](docs/guides/stack-budget.md) | 协程入口栈预算工具与可选运行诊断 |
| [docs/guides/real-client-acceptance.md](docs/guides/real-client-acceptance.md) | 真实客户端验收场景说明（发布 Gate） |
| [docs/guides/ci.md](docs/guides/ci.md) | CI 流水线使用指南（唯一 CI 落点） |
| [.github/skills/bbtools-coroutine/SKILL.md](.github/skills/bbtools-coroutine/SKILL.md) | Agent 写调用代码的入口 |

## 安装

依赖：

- Boost >= 1.90（本库仅用 `boost.context`；CMake 以 `find_package(Boost 1.90 REQUIRED COMPONENTS context)` 强制校验，版本不足会在 configure 阶段直接报错）。发行版仓库的 `apt install libboost-all-dev` 通常不提供足够新的版本，请以实际安装的 Boost 版本为准。
- bbtools-core 不再需要：本仓已内置所需的 `bbt/core` 子集与 `bbt/pollevent`。

构建与安装：

```shell
git clone https://github.com/yqm-307/bbtools-coroutine.git
cd bbtools-coroutine
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
JOBS=$(( $(nproc) / 4 )); JOBS=$(( JOBS < 2 ? 2 : JOBS ))
cmake --build build --parallel "$JOBS"
cd scripts/install && sudo ./install.sh    # 头文件装到 /usr/local/include，库装到 /usr/local/lib
```

CMake 常用开关（布尔项默认全部 `OFF`，完整选项以 `CMakeLists.txt` 为准）：

| 开关 | 作用 |
|------|------|
| `NEED_TEST=ON` | 编译单元测试 |
| `NEED_BENCHMARK=ON` | 编译 `benchmarks/`（含 `unified_stress`） |
| `NEED_EXAMPLE=ON` | 编译 `examples/`；与 `NEED_TEST=ON` 同开时额外注册 `Test_real_clients_echo`（必须 PASS） |
| `PROFILE=ON` | 启用 Profiler |
| `DEBUG_INFO=ON` | 输出库 debug 信息 |
| `STRINGENT_DEBUG=ON` | 严格 debug 模式，性能开销极大 |

## 基础使用

以后台线程模式启动调度器，注册协程并运行：

```cpp
#include <bbt/coroutine/coroutine.hpp>
using namespace bbt::coroutine;

int main()
{
    // 启动调度器
    g_scheduler->Start();

    // 创建协程 - 基础语法
    bbtco [](){
        printf("i am coroutine %lu\n", GetLocalCoroutineId());
        bbtco_yield;  // 让出CPU控制权
        printf("i am coroutine %lu after yield\n", GetLocalCoroutineId());
    };

    // 创建协程 - 带描述
    bbtco_desc("worker") [](){
        bbtco_sleep(100);  // 协程睡眠100ms
    };

    sleep(1);
    // 无停机入口：进程寿命模型下调度器随进程退出，不需要 Stop
    return 0;
}
```

Chan、CoCond、CoMutex、CoPool、CoSelect、CoRWMutex、WaitFor、defer、Hook 等完整示例见[使用手册](docs/guides/user-guide.md)；完整签名与返回码见 [API 参考](docs/guides/api-reference.md)。

进程寿命与生命周期契约（`Scheduler::Stop` 已删除、运行时为进程寿命对象、worker 扩缩容、静态退出期不析构等）与线程模型 / 网络 I/O 域边界，以[核心运行时契约](docs/architecture/core-runtime-contract.md) §6、§11 为唯一真源，README 不再复述。

## 测试

```shell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEED_TEST=ON -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
JOBS=$(( $(nproc) / 4 )); JOBS=$(( JOBS < 2 ? 2 : JOBS ))
cmake --build build --parallel "$JOBS"
ctest --test-dir build --output-on-failure        # 套件数以 `ctest -N` 输出为准
```

冒烟、长时 soak、真实客户端验收与本地验证阶梯见[开发与发布流程](docs/guides/development.md)。
