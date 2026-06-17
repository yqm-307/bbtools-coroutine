// cocond_queue_stress.cc — 测 CoCond 队列纯吞吐（无 sleep 限流）
// 用法: CO_PROC_THREADS=8 ./cocond_queue_stress [dur_s] [nwaiter]
//
// 与 unified_stress 中 stress_cocond 的区别：
//   unified_stress: notifier 每 500ms NotifyAll → 天花板 400 ops/s
//   本测试: notifier 用 bbtco_yield 极限频率 → 队列 push/pop 成为瓶颈
//
// 用于 PR #205 的 lockfree → std::queue+mutex 性能对照

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <chrono>
#include <thread>

using namespace bbt::coroutine::sync;

static volatile sig_atomic_t g_running = 1;
static std::shared_ptr<CoCond> g_cond;
static std::atomic<uint64_t> g_ops{0};

void on_signal(int) { g_running = 0; }

int main(int argc, char** argv) {
    int dur = argc > 1 ? atoi(argv[1]) : 10;
    int nwaiter = argc > 2 ? atoi(argv[2]) : 200;

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    g_scheduler->Start();
    g_cond = bbtco_make_cocond();

    // Waiters: Wait → ops++ → 立即重入（无 yield，无 sleep）
    for (int i = 0; i < nwaiter; ++i)
        bbtco_ref { while (g_running) { g_cond->Wait(); g_ops++; }; };

    // Notifier: 极限频率 NotifyAll（bbtco_yield 让权给调度器）
    bbtco_ref { while (g_running) { g_cond->NotifyAll(); bbtco_yield; }; };

    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(dur));
    g_running = 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto t1 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    uint64_t ops = g_ops.load();

    g_scheduler->Stop();
    printf("ops=%lu elapsed=%.2fs ops/s=%.0f\n", ops, elapsed, ops / elapsed);
    return 0;
}
