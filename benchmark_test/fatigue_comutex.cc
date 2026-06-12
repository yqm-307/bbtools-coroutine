#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>
#include "fatigue_metrics.h"
#include <csignal>

using namespace bbt::coroutine::sync;

const int nco_num = 100;
auto mutex = bbtco_make_comutex();
volatile int a = 0;
volatile int b = 0;

static fatigue::Metrics g_metrics("comutex");
static volatile sig_atomic_t g_running = 1;

void on_signal(int) {
    g_running = 0;
    g_metrics.stop_reporter();
    _exit(0);
}

void fatigue_1()
{
    // ── 100 个协程做 Lock/UnLock (互斥写入) ──
    for (int i = 0; i < nco_num; ++i) {
        bbtco [i]() {
            while (true) {
                uint64_t t0 = fatigue::now_us();
                mutex->Lock();
                uint64_t lat = fatigue::now_us() - t0;
                g_metrics.lock_ops++;
                g_metrics.record_lock_latency_us(lat);

                Assert(a == b);
                a++; b++;
                g_metrics.ops_total++;

                mutex->UnLock();
            }
        };
    }

    // ── 5 个协程做 TryLock(10ms) ──
    for (int i = 0; i < 5; ++i) {
        bbtco [i]() {
            while (true) {
                uint64_t t0 = fatigue::now_us();
                int ret = mutex->TryLock(10);
                uint64_t lat = fatigue::now_us() - t0;

                if (ret == 0) {
                    g_metrics.trylock_success++;
                    g_metrics.record_lock_latency_us(lat);
                    Assert(a == b);
                    a++; b++;
                    g_metrics.ops_total++;
                    mutex->UnLock();
                } else {
                    g_metrics.trylock_timeout++;
                }
            }
        };
    }

    g_metrics.start_reporter(60);  // 每分钟输出一次指标

    // 信号处理：SIGTERM/SIGINT 时优雅退出
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    // 主线程阻塞，让 reporter 和协程持续运行
    while (g_running) {
        sleep(1);
    }

    // unreachable, but clean shutdown for completeness
    g_metrics.stop_reporter();
}

int main()
{
    g_scheduler->Start();
    fatigue_1();
    g_scheduler->Stop();
}
