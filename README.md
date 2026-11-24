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

## 从示例开始

先看 [可运行示例](examples/README.md)：从启动一个协程并等待它完成开始，再按任务选 async、通道、锁、协程池或 I/O 示例。索引提供构建命令和预期输出。

## 文档地图

| 文档 | 内容 |
|------|------|
| [docs/guides/user-guide.md](docs/guides/user-guide.md) | 使用指南：入口选择、场景用法、生命周期、性能与常见错误 |
| [docs/guides/async-usage.md](docs/guides/async-usage.md) | async / bind / await / fire：语法糖与常见场景 |
| [docs/guides/api-reference.md](docs/guides/api-reference.md) | API 参考：签名、返回码、挂起条件与生命周期 |
| [docs/architecture/core-runtime-contract.md](docs/architecture/core-runtime-contract.md) | 核心运行时契约（唯一契约真源）：进程寿命、线程模型、Hook 边界、driver 分层 |
| [docs/guides/development.md](docs/guides/development.md) | 开发与发布流程：验证阶梯、本地入口、发布 Gate |
| [docs/guides/performance.md](docs/guides/performance.md) | 性能问题分析与优化流程、测量协议 |
| [docs/guides/stack-budget.md](docs/guides/stack-budget.md) | 协程入口栈预算工具与可选运行诊断 |
| [docs/guides/real-client-acceptance.md](docs/guides/real-client-acceptance.md) | 真实客户端验收场景说明（发布 Gate） |
| [docs/guides/ci.md](docs/guides/ci.md) | CI 流水线使用指南（唯一 CI 落点） |
| [bbtools-pub README](https://github.com/yqm-307/bbtools-pub/blob/main/README.md) | 跨仓设计、编码、Review 与用例设计规范入口（固定 submodule） |
| [.github/skills/bbtools-coroutine/SKILL.md](.github/skills/bbtools-coroutine/SKILL.md) | Agent 写调用代码的入口 |

## 安装

依赖：

- Boost >= 1.90（本库仅用 `boost.context`；CMake 以 `find_package(Boost 1.90 REQUIRED COMPONENTS context)` 强制校验，版本不足会在 configure 阶段直接报错）。发行版仓库的 `apt install libboost-all-dev` 通常不提供足够新的版本，请以实际安装的 Boost 版本为准。
- bbtools-core 不再需要：本仓已内置所需的 `bbt/core` 子集与 `bbt/pollevent`。

构建与安装：

```shell
git clone https://github.com/yqm-307/bbtools-coroutine.git
cd bbtools-coroutine
git submodule update --init --recursive
./scripts/install/install.sh    # 构建到 build-install 并安装到 ~/.local，无需 root
```

前缀默认 `~/.local`，`BBT_INSTALL_PREFIX` 可覆盖；并行度 `BBT_JOBS`（默认 2）、构建目录 `BBT_BUILD_DIR` 同理。`scripts/install/uninstall.sh` 按同一前缀卸载。

需要 ccache 或自定义并行度的完整构建仍可手动执行：

```shell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
flock /tmp/bbt-build.lock cmake --build build --parallel 2
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

进程寿命与生命周期契约（`Scheduler::Stop` 已删除、运行时为进程寿命对象、worker 扩缩容、静态退出期不析构等）与线程模型 / 网络 I/O 域边界，以[核心运行时契约](docs/architecture/core-runtime-contract.md) §6、§11 为唯一真源，README 不再复述。

## 测试

```shell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEED_TEST=ON -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
flock /tmp/bbt-build.lock cmake --build build --parallel 2
bash scripts/ci/run_test_profile.sh --build-dir build --profile smoke   # 全量 smoke；套件数以 `ctest -N` 为准
```

冒烟、长时 soak、真实客户端验收与本地验证阶梯见[开发与发布流程](docs/guides/development.md)。
