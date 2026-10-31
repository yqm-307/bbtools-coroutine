# bbtools-coroutine 使用手册

调用本库的说明按主题组织，与仓库 skill 相同。

| 模块 | 路径 |
|------|------|
| 架构简介 | `.github/skills/bbtools-coroutine/references/architecture.md` |
| 基本用法 / 进阶用法 / 使用范式 | `.github/skills/bbtools-coroutine/references/usage.md` |
| API 参考（仅当前事实） | `docs/guides/api-reference.md` |
| 显式协程声明与调用（`bbt_co_async(T)` / `bbt_co_await`） | [API 参考 §2](api-reference.md#2-协程注册与调用语法) |
| 坑点与禁止行为 | `.github/skills/bbtools-coroutine/references/pitfalls.md` |
| 执行域与网络 I/O 域边界（目标架构） | `docs/architecture/core-runtime-contract.md` §11 |

## P 本地调度与升级注意事项

每个 P 使用一个 CFS 就绪集合：新提交、global 补入、yield 和事件唤醒均参与相同排序；同等虚拟服务量按入队先后选择。记账随 Coroutine 保存，跨 P 偷取或退役经 global 中转时按两端本地基准重定位，不清零历史债务。唤醒不积攒无限等待信用。

`m_cfg_processer_priority_runtime_budget_us` 沿用名称和默认 `{50, 150, 200, 600}`，含义已改为 LOW/NORMAL/HIGH/CRITICAL 的相对服务权重，按 `max(1, elapsed_us) / max(1, weight)` 推进虚拟服务量；旧配置中的零预算不再禁用某优先级。这是配置语义变化，依赖旧预算含义的调用方需要调整；配置仍须在 Start 前完成。

CFS 仍是协作式调度：单次 Resume 不被强制抢占，业务长期不让出时不能承诺等待上界。P 队列内部使用短 mutex，锁不跨 Resume、park/yield 发布或外部 driver Wake；全局队列与通知协议保持既有边界。Coroutine/Processer 内部布局发生变化，升级后须重编本库及直接依赖，不混用旧头文件和二进制。

Agent 加载 `.github/skills/bbtools-coroutine/SKILL.md`。改运行时实现仍读 `AGENTS.md` 与 `docs/architecture/core-runtime-contract.md`。涉及网络 I/O 集成先读契约 §11。

## 基础用法示例

完整签名、返回码与生命周期见 [API 参考](api-reference.md)；进程寿命与生命周期契约见[核心运行时契约](../architecture/core-runtime-contract.md) §6。

### 协程通道（Chan）

Chan 支持阻塞读写：`Write`/`Read` 返回 0 成功、-1 关闭（`Close()` 后须读空缓冲才返回 -1）、-2 错误；`TryWrite`/`TryRead` 立即返回，带毫秒参数的版本等待至超时（超时返回码以 API 参考为准）；`Close()` 后仍可读尽缓冲数据；`IsClosed()` 查询关闭状态：

```cpp
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
using namespace bbt::coroutine;

void ChanExample()
{
    auto chan = sync::Chan<int, 1>{};
    bbtco [&](){ chan << 42; };
    bbtco [&](){
        int value = 0;
        chan >> value;
        printf("read value: %d\n", value);
    };
    sleep(1);
}
```

多写者模式：创建多个写者协程向同一 `Chan<int, 10>` 写入，读者协程按序读出。

### 协程条件变量（CoCond）

```cpp
auto cocond = bbtco_make_cocond();
bbtco [&](){ cocond->Wait(); };              // 挂起当前协程
bbtco [&](){ bbtco_sleep(500); cocond->NotifyOne(); };   // 唤醒一个等待者
// cocond->NotifyAll();                     // 唤醒全部等待者
```

### 协程互斥锁（CoMutex）

```cpp
#include <bbt/coroutine/sync/CoLockGuard.hpp>
auto comutex = bbtco_make_comutex();
bbtco [&](){
    sync::CoLockGuard<sync::CoMutex> lock(comutex);  // RAII：构造加锁、析构解锁
    // 临界区（Lock() 返回 void，不能接返回值）
};
```

### 协程池（CoPool）

```cpp
auto co_pool = bbtco_make_copool(2);
co_pool->Submit([]() { bbtco_sleep(500); });
// 等待运行中的任务退出；队列中未执行的任务被取消式排空
//（带 future 的任务以 broken_promise 兑现）
co_pool->Release();
```

### 事件等待（WaitFor）

```cpp
#include <unistd.h>
int pipefd[2];
pipe(pipefd);
bbtco [&](){
    bbtco_wait_for(pipefd[0], bbtco_emev_readable, 0);  // 等待 fd 可读
    char buffer[64] = {0};
    read(pipefd[0], buffer, sizeof(buffer));
};
bbtco [](){ bbtco_wait_for(0, bbtco_emev_timeout, 500); };  // 纯超时 500ms
```

### Defer 语义

```cpp
bbtco [](){
    FILE* file = fopen("test.txt", "w");
    if (file) {
        bbtco_defer { fclose(file); };   // 逆序执行
        fprintf(file, "Hello World\n");
    }
};
```

### 多路复用（CoSelect）

Go select 风格的多 Chan 复用，返回命中 Case 的注册序下标（-1 = 超时 / Default / 唤醒后均未就绪）：

```cpp
#include <bbt/coroutine/sync/CoSelect.hpp>
using namespace bbt::coroutine::sync;
auto ch1 = sync::Chan<int, 4>{};
auto ch2 = sync::Chan<int, 4>{};
ch1.TryWrite(1);
int a = 0, b = 0;
int idx = CoSelect().CaseRead(ch1, a).CaseRead(ch2, b).CaseTimeout(1000).Run();
```

支持 `CaseRead` / `CaseWrite` / `CaseTimeout` / `Default`；只支持缓冲 Chan（`Chan<T, 0>` 编译期拦截）。

### 读写锁（CoRWMutex）

`RLock`/`WLock` 阻塞加锁，`TryRLock`/`TryWLock` 非阻塞或带毫秒超时，配合 RAII 守卫 `CoReadLock`/`CoWriteLock`：

```cpp
auto rwmutex = bbtco_make_corwmutex();
bbtco [&](){ CoReadLock guard(rwmutex); bbtco_sleep(100); };
bbtco [&](){ CoWriteLock guard(rwmutex); };
```

### Hook / I/O

协程内调用常见阻塞 syscall 时，Hook 将其转为协程等待，不占死调度线程；非协程上下文直通原生实现，第三方同步客户端（如 hiredis）可原样复用。

已拦截：`socket`、`connect`、`accept`/`accept4`、`send`/`sendto`/`sendmsg`、`recv`/`recvfrom`/`recvmsg`、`read`/`readv`、`write`/`writev`、`close`、`poll`/`select`/`pselect`、`sleep`/`usleep`/`nanosleep`/`clock_nanosleep`、`getaddrinfo`/`getnameinfo`/`gethostbyname`/`gethostbyaddr`。

- 传入 blocking FD 无需自行改造：协程 IO 期间临时 `O_NONBLOCK`，返回时恢复原 flags；
- `MSG_DONTWAIT` 直通原生；`SO_RCVTIMEO`/`SO_SNDTIMEO` 保持有界返回 `-1/EAGAIN`；
- 等待中的 FD 被 `close` 时唤醒等待协程并返回 `EBADF`，不会永久挂起；
- 语义边界与迁移注意见契约文档。

契约测试：`Test_hook_contract`、`Test_hook_blocking_fd`、`Test_hook_timeout_flags`、`Test_hook_error_matrix`。

### 综合示例：生产者-消费者

```cpp
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
#include <atomic>
#include <memory>
using namespace bbt::coroutine;

struct Task {
    int id;
    std::string data;
    Task(int i, const std::string& d) : id(i), data(d) {}
};

void ProducerConsumerExample()
{
    auto task_chan = sync::Chan<Task, 10>();
    auto worker_pool = bbtco_make_copool(3);
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};

    for (int i = 0; i < 2; ++i) {
        bbtco_desc("producer") [&, i](){
            for (int j = 0; j < 5; ++j) {
                task_chan << Task(i * 100 + j, "data");
                produced++;
                bbtco_sleep(10);
            }
        };
    }
    for (int i = 0; i < 3; ++i) {
        bbtco_desc("consumer") [&, i](){
            while (true) {
                Task task(0, "");
                if (!(task_chan >> task)) break;
                worker_pool->Submit([task, i, &consumed]() {
                    bbtco_defer { consumed++; };
                    bbtco_sleep(10);
                });
            }
        };
    }
    bbtco_desc("closer") [&](){
        while (produced < 10) bbtco_sleep(100);
        task_chan.Close();
    };
    sleep(3);
    worker_pool->Release();
}
```

## 注意事项

### 协程生命周期管理

- 协程创建后立即被调度执行；协程函数结束时自动销毁。
- 避免协程函数中使用栈上的引用，除非能确保生命周期。

### 内存管理

- 协程栈大小可按协程指定：省略时取 `m_cfg_stack_size`（出厂 **12 KiB**，不是 2 MB），显式值向上按系统页对齐后生效。
- 不同大小的固定栈可同时共存（分桶键 = 对齐后可用字节数 + 保护属性）；栈大小创建后固定，不支持扩栈/换栈。
- 显式申请 0 或超出可表示范围会被拒绝（`invalid_argument` / `length_error`），内存不足是 `bad_alloc`，建立保护页失败是带 `errno` 的 `system_error`；用 `bbtco_noexcept(succ)` / `bool& succ` 入口可拿到不抛的失败返回。
- 避免在协程中分配大量栈空间；在极小栈上跑日志/断言/第三方库这类重活要先给足栈。
- 使用智能指针管理堆内存。

### 异常处理

- 协程内异常不会传播到主线程：detached 协程保存 `exception_ptr` 可取回；无异常回调时记日志并计数，不静默吞、不 terminate。
- 生产环境建议设置异常回调（`m_ext_coevent_exception_callback`）统一收口。
- 使用 defer 进行资源清理。

### 性能优化

- 避免在协程池中提交大量长时间 CPU 计算任务（会占住 worker）。
- 合理设置 Chan 的缓冲区大小。
- 使用引用捕获避免不必要的拷贝。

### 调试技巧

- 使用 `bbtco_desc` 为协程添加描述信息（诊断现场可读回）。
- 通过 `GetLocalCoroutineId()` 追踪协程执行。
- 阻塞 syscall 会被 Hook 转成协程等待，无需刻意规避；纯 CPU 长任务会占住 worker，可开启 worker stall 告警（`m_cfg_worker_stall_warn_ms`）发现。
