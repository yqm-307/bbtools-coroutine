// micro_co_switch.cc — 协程上下文切换延迟微基准
// 用法: ./micro_co_switch [iterations] [threads]
// 输出: JSON 报告到 stdout
//
// 测量单协程 yield→resume 往返延迟（含调度器开销）。
// 契约参考: docs/testing-contract-spec.md §2.2

#include <bbt/coroutine/coroutine.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

static volatile bool g_done = false;
static uint64_t g_total_ns = 0;
static uint64_t g_iterations = 100000;
static uint64_t g_measured = 0;

// 单协程 yield 循环：每次 yield 到 resume 为一个往返
static void co_switch_loop()
{
    using clock = std::chrono::steady_clock;
    uint64_t warmup = g_iterations / 100 > 0 ? g_iterations / 100 : 1;
    uint64_t total_ns = 0;
    uint64_t count = 0;

    for (uint64_t i = 0; i < g_iterations; ++i)
    {
        auto t0 = clock::now();
        bbtco_yield;
        auto t1 = clock::now();
        uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

        if (i >= warmup)
        {
            total_ns += ns;
            count++;
        }
    }

    g_total_ns = total_ns;
    g_measured = count;
    g_done = true;
}

int main(int argc, char **argv)
{
    if (argc >= 2)
        g_iterations = (uint64_t)atoll(argv[1]);
    int proc_threads = 0;
    if (argc >= 3)
        proc_threads = atoi(argv[2]);
    if (proc_threads > 0)
        g_bbt_coroutine_config->m_cfg_static_thread_num = (size_t)proc_threads;

    size_t nthreads = g_bbt_coroutine_config->m_cfg_static_thread_num;
    printf("[micro_co_switch] iterations=%lu threads=%zu\n",
           (unsigned long)g_iterations, nthreads);

    // ── Phase 1: 单协程 yield 往返延迟 ──
    printf("[micro_co_switch] phase 1: single-co yield round-trip...\n");
    g_scheduler->Start();

    // 使用 CountDownLatch 同步等待 (避免 sleep_for 被 hook)
    auto latch = std::make_shared<bbt::core::thread::CountDownLatch>(1);

    bbtco_ref
    {
        co_switch_loop();
        latch->Down();
    };

    // 等待完成 — 用 busy-poll 避免 sleep hook
    latch->Wait();

    g_scheduler->Stop();

    double avg_ns = g_measured > 0 ? (double)g_total_ns / g_measured : 0;

    // ── Phase 2: 协程创建吞吐 ──
    printf("[micro_co_switch] phase 2: co spawn throughput...\n");
    const uint64_t spawn_count = g_iterations < 10000 ? g_iterations : 10000;
    std::atomic<uint64_t> spawn_done{0};

    g_scheduler->Start();

    auto spawn_t0 = std::chrono::steady_clock::now();

    for (uint64_t i = 0; i < spawn_count; ++i)
    {
        bool succ = false;
        while (!succ)
        {
            bbtco_noexcept(&succ)
            [&spawn_done]()
            {
                spawn_done++;
            };
            if (!succ)
                bbtco_sleep(1);
        }
    }

    struct timespec ts = {0, 10 * 1000 * 1000}; // 10ms
    while (spawn_done.load() < spawn_count)
        nanosleep(&ts, nullptr);

    auto spawn_t1 = std::chrono::steady_clock::now();
    uint64_t spawn_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(spawn_t1 - spawn_t0).count();
    double spawn_avg_ns = (double)spawn_ns / spawn_count;

    g_scheduler->Stop();

    // ── JSON 输出 ──
    fprintf(stdout,
            "BENCH_RESULT:{"
            "\"name\":\"micro_co_switch\","
            "\"iterations\":%lu,"
            "\"measured_iterations\":%lu,"
            "\"threads\":%zu,"
            "\"yield_roundtrip_avg_ns\":%.0f,"
            "\"co_spawn_count\":%lu,"
            "\"co_spawn_avg_ns\":%.0f"
            "}\n",
            (unsigned long)g_iterations,
            (unsigned long)g_measured,
            nthreads,
            avg_ns,
            (unsigned long)spawn_count,
            spawn_avg_ns);

    // 人类可读摘要
    printf("[micro_co_switch] yield round-trip: %.0f ns (%.2f μs)\n", avg_ns, avg_ns / 1000.0);
    printf("[micro_co_switch] co spawn:        %.0f ns (%.2f μs)\n", spawn_avg_ns, spawn_avg_ns / 1000.0);
    printf("[micro_co_switch] done\n");

    return 0;
}
