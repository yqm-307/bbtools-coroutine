/**
 * 内存检测（Valgrind memcheck）专用工作负载。
 *
 * 与 .github/workflows/memery_test_info.yml、scripts/ci/run_memcheck.py、
 * docs/ci-guide.md §4 配套；本文件只负责“把真实负载跑完并留下可核证据”。
 *
 * 完成协议（业务计数完成 ≠ 运行时对象已释放，两者必须分开证明）：
 * 1. 业务层：所有 CountDownLatch->Wait() 只在主线程调用；协程内只做 Down()，
 *    且 Down() 是用户函数体的最后一条语句。Wait() 返回只代表“业务完成信号已收齐”，
 *    不代表用户函数已返回、Coroutine 对象已析构或栈已归还。
 * 2. 运行时层：唯一读取栈池账目 g_bbt_stackpoll->GetCurCoNum()（= 已创建栈数 -
 *    池内闲置数）。Context 是唯一的栈借出方（Context.cc: m_stack(Apply())），
 *    ~Context 才 Release 归还，而 ~Context 由 Processer FINAL delete
 *    Coroutine 触发。故 GetCurCoNum()==0 只证明“所有借出的栈都已归还”
 *    （即 ~Context 已进入 FINAL 释放路径）。
 *    #379 已采用 A：~Context 函数体先清空 m_onyield_callback、m_user_main，令这两个
 *    callable 及其最后持有的捕获对象同步析构，之后才 Release(m_stack)。因此
 *    cur==0 蕴含“被这两个 callback 独占持有的业务资源已析构”，这是本协议能证明的
 *    精确上限。它**不**证明：外部其它 owner 持有的资源已释放、worker 侧异步物理
 *    清理完成、delete Coroutine 已返回或对象本体 deallocation——delete 发生在
 *    worker 线程，main 线程看到 cur==0 即返回，与其没有 happens-before。
 *    故 runtime_drained=1 不得表述为“Coroutine/Context 与业务资源已完整释放”。
 * 3. 检测前：主线程确认 runtime_drained 后调用 StackPool::ReleaseUnused，释放池中
 *    为复用保留的闲置栈；stack_pool_drained=1 才允许进入 memcheck 判定。
 * 4. 该计数无条件维护（StackPool::_AllocItem/_FreeItem 与 PROFILE 无关），
 *    不依赖 PROFILE=OFF 时会失效的 Profiler 计数，不会出现“恒 0 假完成”。
 *    有界等待，超时 = 未完成 = FAIL，绝不靠延长等待或缩短负载变绿。
 * 5. 低于此规模不允许：10000 个嵌套协程、CoMutex 竞争、10 个 channel
 *    各 1 个合法 reader + 5 writers × 10000（合计 500000 次读取）。
 *
 * 并发约束（现场教训，不得回退）：协程内绝不调用阻塞式 Wait/WaitTimeout
 * （它是 pthread wait，会占死 worker）；只用会挂起协程的 Chan/CoMutex 原语。
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/StackPool.hpp>
#include <bbt/coroutine/sync/CoLockGuard.hpp>

namespace
{

using bbt::core::thread::CountDownLatch;
using LatchSPtr = std::shared_ptr<CountDownLatch>;

/* 固定 worker 数，保证调度行为与 runner 核数无关 */
constexpr size_t kWorkerNum = 2;

/* 阶段一：嵌套协程，覆盖创建/切换/每协程独立栈使用（规模同基线 7bcda3b） */
constexpr int kOuterCoroutines = 1;
constexpr int kInnerPerOuter   = 10000;
constexpr int kInnerTotal      = kOuterCoroutines * kInnerPerOuter;

/* 阶段二：CoMutex 竞争，覆盖挂起/唤醒 */
constexpr int kMutexCoroutines  = 8;
constexpr int kLockPerCoroutine = 500;
constexpr long long kLockTotal  = static_cast<long long>(kMutexCoroutines) * kLockPerCoroutine;

/* 阶段三：多 channel 并行 + 阻塞/挂起/唤醒（不得低于旧版负载量级） */
constexpr int kChanNum        = 10;
constexpr int kWriterPerChan  = 5;
constexpr int kWritePerWriter = 10000;
constexpr int kWritePerChan   = kWriterPerChan * kWritePerWriter;   // 每 channel 50000
constexpr int kChanCapacity   = 100;
constexpr int kTotalMessages  = kChanNum * kWritePerChan;           // 500000

static_assert(kChanCapacity > 0, "buffered channel 容量必须为正");
static_assert(kChanCapacity < kWritePerChan,
    "容量必须小于单 channel 消息总量，writer 才会真实阻塞并被 reader 唤醒");

std::atomic_int    g_inner_done{0};
std::atomic_llong  g_chan_read{0};
std::atomic_int    g_chan_writer_done{0};
std::atomic_int    g_failures{0};   // Write/Read 非 0、越界、lost-wakeup 等显式失败

/* 协程内的确定性计算量：只读写本协程栈上的缓冲，不触碰共享可变状态 */
int StackWork(uint64_t seed)
{
    volatile uint8_t buf[512];
    uint32_t acc = static_cast<uint32_t>(seed);
    for (size_t i = 0; i < sizeof(buf); ++i) {
        buf[i] = static_cast<uint8_t>(seed + i);
        acc = acc * 16777619u ^ buf[i];
    }
    return static_cast<int>(acc & 0x7fffffffu);
}

/* 运行时层完成信号：栈池无借出（全部已归还） */
bool IsRuntimeDrained(int& alloc_out, int& cur_out)
{
    alloc_out = g_bbt_stackpoll->AllocSize();
    cur_out   = g_bbt_stackpoll->GetCurCoNum();
    return cur_out == 0;
}

/* 有界等待运行时收口；超时是“未完成”，返回 false 让主流程 FAIL（fail-closed） */
bool WaitForRuntimeDrain(int timeout_ms, int& alloc_out, int& cur_out)
{
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms);
    while (!IsRuntimeDrained(alloc_out, cur_out)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr,
                "BBT_MEMCHECK_FAILED runtime drain 超时：alloc=%d cur=%d"
                "（仍有借出未归还的栈，检测时点不可信）\n",
                alloc_out, cur_out);
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

void RunNestedCoroutines()
{
    /* 主线程等待（合法位置）；协程只 Down */
    LatchSPtr outer_latch = std::make_shared<CountDownLatch>(kOuterCoroutines);
    LatchSPtr inner_latch = std::make_shared<CountDownLatch>(kInnerTotal);

    for (int o = 0; o < kOuterCoroutines; ++o) {
        bbtco [outer_latch, inner_latch, o]() {
            for (int i = 0; i < kInnerPerOuter; ++i) {
                bbtco [inner_latch, o, i]() {
                    const int r = StackWork(static_cast<uint64_t>(o) * kInnerPerOuter + i);
                    (void)r;
                    g_inner_done.fetch_add(1, std::memory_order_relaxed);
                    inner_latch->Down();   // 用户函数体最后一条语句
                };
            }
            outer_latch->Down();
        };
    }

    outer_latch->Wait();
    inner_latch->Wait();
    std::fprintf(stderr, "[bbt-mc] stage1 nested done inner=%d/%d\n",
                 g_inner_done.load(), kInnerTotal);
}

long long RunCoMutex()
{
    auto mutex   = bbt::coroutine::sync::CoMutex::Create();
    auto counter = std::make_shared<std::atomic<long long>>(0);
    LatchSPtr latch = std::make_shared<CountDownLatch>(kMutexCoroutines);

    for (int i = 0; i < kMutexCoroutines; ++i) {
        bbtco [latch, counter, mutex]() {
            for (int j = 0; j < kLockPerCoroutine; ++j) {
                bbt::coroutine::sync::CoLockGuard<bbt::coroutine::sync::CoMutex> guard{mutex};
                counter->fetch_add(1, std::memory_order_relaxed);
            }
            latch->Down();
        };
    }

    latch->Wait();
    std::fprintf(stderr, "[bbt-mc] stage2 comutex done lock=%lld/%lld\n",
                 counter->load(), kLockTotal);
    return counter->load();
}

/* 单 channel：5 writer（缓冲满时真实阻塞挂起）+ 1 合法 reader。
 * reader 阻塞于空队列时由 writer 唤醒；这是本负载要覆盖的协作路径。 */
void RunOneChan(int chan_index,
                const std::shared_ptr<bbt::coroutine::sync::Chan<int, kChanCapacity>>& chan,
                const LatchSPtr& writer_latch, const LatchSPtr& done_latch)
{
    for (int w = 0; w < kWriterPerChan; ++w) {
        bbtco [chan, writer_latch, chan_index, w]() {
            for (int i = 0; i < kWritePerWriter; ++i) {
                if (chan->Write(chan_index * kWritePerChan + w * kWritePerWriter + i) != 0) {
                    std::fprintf(stderr, "[bbt-mc] chan[%d] writer[%d] failed at %d\n",
                                 chan_index, w, i);
                    g_failures.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
            g_chan_writer_done.fetch_add(1, std::memory_order_relaxed);
            writer_latch->Down();
        };
    }

    bbtco [chan, done_latch, chan_index]() {
        int local_read = 0;
        while (local_read < kWritePerChan) {
            int val = 0;
            const int r = chan->Read(val);
            if (r != 0) {
                /* Close 由主线程在收齐 writer 后执行；reader 收边前见到非 0
                 * 只可能是丢消息/关闭，记失败退出，不静默吞掉。 */
                std::fprintf(stderr, "[bbt-mc] chan[%d] reader failed(%d) at %d/%d\n",
                             chan_index, r, local_read, kWritePerChan);
                g_failures.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            const int upper = (chan_index + 1) * kWritePerChan - 1;
            if (val < chan_index * kWritePerChan || val > upper) {
                std::fprintf(stderr, "[bbt-mc] chan[%d] reader out-of-range val=%d\n",
                             chan_index, val);
                g_failures.fetch_add(1, std::memory_order_relaxed);
            }
            ++local_read;
            g_chan_read.fetch_add(1, std::memory_order_relaxed);
        }
        done_latch->Down();
    };
}

void RunChanFanOut()
{
    /* writer_latch 总数 = 10ch × 5w = 50，done_latch 总数 = 10 reader，均由主线程收齐 */
    LatchSPtr writer_latch = std::make_shared<CountDownLatch>(kChanNum * kWriterPerChan);
    LatchSPtr done_latch   = std::make_shared<CountDownLatch>(kChanNum);

    std::vector<std::shared_ptr<bbt::coroutine::sync::Chan<int, kChanCapacity>>> chans;
    chans.reserve(kChanNum);
    for (int c = 0; c < kChanNum; ++c) {
        chans.push_back(std::make_shared<bbt::coroutine::sync::Chan<int, kChanCapacity>>());
        RunOneChan(c, chans.back(), writer_latch, done_latch);
    }

    writer_latch->Wait();
    std::fprintf(stderr, "[bbt-mc] stage3 writers done=%d readers=%lld\n",
                 g_chan_writer_done.load(), g_chan_read.load());

    for (auto& ch : chans)
        ch->Close();   // 主线程收边：缓冲不丢弃，reader 继续读满后正常退出

    done_latch->Wait();
    std::fprintf(stderr, "[bbt-mc] stage3 chan done read=%lld\n", g_chan_read.load());
}

} // namespace

int main()
{
    /* worker 数必须在 Start 之前设置（Start 一次性，成功后不可重置配置） */
    g_bbt_coroutine_config->m_cfg_static_thread_num = kWorkerNum;

    g_scheduler->Start();

    RunNestedCoroutines();
    const long long lock_total = RunCoMutex();
    RunChanFanOut();

    /* ===== 完成协议：业务层证据 + 运行时收口信号（fail-closed）===== */
    const int       inner_done  = g_inner_done.load();
    const long long chan_read   = g_chan_read.load();
    const int       writer_done = g_chan_writer_done.load();
    const int       failures    = g_failures.load();

    int alloc = 0;
    int cur   = 0;
    const bool runtime_drained = WaitForRuntimeDrain(/*timeout_ms=*/30000, alloc, cur);
    const size_t released_stacks = runtime_drained
        ? g_bbt_stackpoll->ReleaseUnused() : 0;
    alloc = g_bbt_stackpoll->AllocSize();
    cur   = g_bbt_stackpoll->GetCurCoNum();
    const bool stack_pool_drained = runtime_drained && alloc == 0 && cur == 0;

    const bool business_done =
        (inner_done  == kInnerTotal) &&
        (lock_total  == kLockTotal) &&
        (chan_read   == kTotalMessages) &&
        (writer_done == kChanNum * kWriterPerChan) &&
        (failures    == 0);

    const bool ok = business_done && runtime_drained && stack_pool_drained;

    /* 可核完成标记：run_memcheck.py 据此判定程序真的跑完。缺必需键、键重复、
     * 出现多条互相冲突的标记、或任一取值与固定期望不符，全部 FAIL。
     * 期望值与 scripts/ci/run_memcheck.py 的 EXPECTED_COUNTERS 必须一致。 */
    std::printf("BBT_MEMCHECK_DONE inner=%d/%d lock=%lld/%lld chan_read=%lld/%d "
                "writer_done=%d/%d failures=%d/%d runtime_drained=%d/%d "
                "stack_pool_drained=%d/%d (released=%zu alloc=%d cur=%d)\n",
                inner_done, kInnerTotal,
                lock_total, kLockTotal,
                chan_read, kTotalMessages,
                writer_done, kChanNum * kWriterPerChan,
                failures, 0,
                static_cast<int>(runtime_drained), 1,
                static_cast<int>(stack_pool_drained), 1,
                released_stacks, alloc, cur);

    if (!ok) {
        std::fprintf(stderr,
                     "BBT_MEMCHECK_FAILED 完成协议不达标（business=%d runtime=%d pool=%d）\n",
                     static_cast<int>(business_done), static_cast<int>(runtime_drained),
                     static_cast<int>(stack_pool_drained));
        return 1;
    }

    /* process-lifetime：无业务停机入口；业务完成后返回 main 即进程退出。
     * 单例按契约由刻意泄漏的持有者维持，不走静态析构顺序。 */
    return 0;
}
