// fatigue_metrics.h — 疲劳测试业务埋点（原子计数器 + JSON 周期输出）
//
// 用法：
//   #include "fatigue_metrics.h"
//   static fatigue::Metrics g_metrics("comutex");
//
//   void worker() {
//       uint64_t t0 = fatigue::now_us();
//       mutex->Lock();
//       uint64_t lat = fatigue::now_us() - t0;
//       g_metrics.lock_ops++;
//       g_metrics.record_lock_latency_us(lat);
//       // ... work ...
//       mutex->UnLock();
//   }
//
//   int main() {
//       g_metrics.start_reporter(60);  // 推荐每分钟 1 个采样点（~800B/次 → ~48KB/h）
//       // ... run fatigue test ...
//       g_metrics.stop_reporter();
//   }
//
// 输出格式（stdout）：
//   FATIGUE_METRIC:{"ts":10.0,"name":"comutex","elapsed_s":10.0,"ops_total":500000,...}

#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace fatigue {

inline uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Metrics {
    std::string name;
    uint64_t start_us;

    // ── 通用 ──
    std::atomic<uint64_t> ops_total{0};
    std::atomic<uint64_t> errors{0};

    // ── CoMutex / std::mutex ──
    std::atomic<uint64_t> lock_ops{0};
    std::atomic<uint64_t> trylock_success{0};
    std::atomic<uint64_t> trylock_timeout{0};
    std::atomic<uint64_t> lock_latency_sum_us{0};
    std::atomic<uint64_t> lock_latency_count{0};

    // ── CoRWMutex ──
    std::atomic<uint64_t> rlock_ops{0};
    std::atomic<uint64_t> wlock_ops{0};
    std::atomic<uint64_t> try_rlock_timeout{0};
    std::atomic<uint64_t> try_wlock_timeout{0};

    // ── CoCond ──
    std::atomic<uint64_t> cond_waits{0};
    std::atomic<uint64_t> cond_signals{0};
    std::atomic<uint64_t> cond_timeouts{0};
    std::atomic<uint64_t> cond_latency_sum_us{0};
    std::atomic<uint64_t> cond_latency_count{0};

    // ── Chan ──
    std::atomic<uint64_t> chan_reads{0};
    std::atomic<uint64_t> chan_writes{0};
    std::atomic<uint64_t> chan_read_blocks{0};
    std::atomic<uint64_t> chan_write_blocks{0};

    // ── CoPool ──
    std::atomic<uint64_t> pool_tasks{0};
    std::atomic<uint64_t> pool_completed{0};
    std::atomic<uint64_t> pool_dropped{0};

    // ── Coroutine ──
    std::atomic<uint64_t> co_spawns{0};
    std::atomic<uint64_t> co_switches{0};

    // ── 控制 ──
    std::atomic<bool> reporter_running{false};
    std::thread reporter_thread;

    explicit Metrics(const std::string& n)
        : name(n), start_us(now_us()) {}

    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;

    double elapsed_s() const {
        return (now_us() - start_us) / 1e6;
    }

    void record_lock_latency_us(uint64_t lat_us) {
        lock_latency_sum_us += lat_us;
        lock_latency_count++;
    }
    void record_cond_latency_us(uint64_t lat_us) {
        cond_latency_sum_us += lat_us;
        cond_latency_count++;
    }

    // ── JSON 输出 ──
    void dump_json() {
        double elapsed = elapsed_s();
        uint64_t ops = ops_total.load();
        uint64_t err = errors.load();
        uint64_t lo = lock_ops.load();
        uint64_t ts_ok = trylock_success.load();
        uint64_t ts_to = trylock_timeout.load();
        uint64_t lsum = lock_latency_sum_us.load();
        uint64_t lcnt = lock_latency_count.load();
        double lock_avg_us = lcnt > 0 ? (double)lsum / lcnt : 0;
        uint64_t rlo = rlock_ops.load();
        uint64_t wlo = wlock_ops.load();
        uint64_t rt_to = try_rlock_timeout.load();
        uint64_t wt_to = try_wlock_timeout.load();
        uint64_t cw = cond_waits.load();
        uint64_t cs = cond_signals.load();
        uint64_t ct = cond_timeouts.load();
        uint64_t csum = cond_latency_sum_us.load();
        uint64_t ccnt = cond_latency_count.load();
        double cond_avg_us = ccnt > 0 ? (double)csum / ccnt : 0;
        uint64_t cr = chan_reads.load();
        uint64_t cw_ch = chan_writes.load();
        uint64_t crb = chan_read_blocks.load();
        uint64_t cwb = chan_write_blocks.load();
        uint64_t pt = pool_tasks.load();
        uint64_t pc = pool_completed.load();
        uint64_t pd = pool_dropped.load();
        uint64_t cs_sp = co_spawns.load();
        uint64_t cs_sw = co_switches.load();

        // fprintf 逐字段输出，避免 snprintf 转义地狱
        fprintf(stdout,
            "FATIGUE_METRIC:{"
            "\"ts\":%.1f,"
            "\"name\":\"%s\","
            "\"elapsed_s\":%.1f,"
            "\"ops_total\":%lu,"
            "\"errors\":%lu,"
            "\"lock_ops\":%lu,"
            "\"trylock_success\":%lu,"
            "\"trylock_timeout\":%lu,"
            "\"lock_avg_us\":%.1f,"
            "\"lock_latency_count\":%lu,"
            "\"rlock_ops\":%lu,"
            "\"wlock_ops\":%lu,"
            "\"try_rlock_timeout\":%lu,"
            "\"try_wlock_timeout\":%lu,"
            "\"cond_waits\":%lu,"
            "\"cond_signals\":%lu,"
            "\"cond_timeouts\":%lu,"
            "\"cond_avg_us\":%.1f,"
            "\"chan_reads\":%lu,"
            "\"chan_writes\":%lu,"
            "\"chan_read_blocks\":%lu,"
            "\"chan_write_blocks\":%lu,"
            "\"pool_tasks\":%lu,"
            "\"pool_completed\":%lu,"
            "\"pool_dropped\":%lu,"
            "\"co_spawns\":%lu,"
            "\"co_switches\":%lu"
            "}\n",
            elapsed,
            name.c_str(),
            elapsed,
            (unsigned long)ops,
            (unsigned long)err,
            (unsigned long)lo,
            (unsigned long)ts_ok,
            (unsigned long)ts_to,
            lock_avg_us,
            (unsigned long)lcnt,
            (unsigned long)rlo,
            (unsigned long)wlo,
            (unsigned long)rt_to,
            (unsigned long)wt_to,
            (unsigned long)cw,
            (unsigned long)cs,
            (unsigned long)ct,
            cond_avg_us,
            (unsigned long)cr,
            (unsigned long)cw_ch,
            (unsigned long)crb,
            (unsigned long)cwb,
            (unsigned long)pt,
            (unsigned long)pc,
            (unsigned long)pd,
            (unsigned long)cs_sp,
            (unsigned long)cs_sw);

        fflush(stdout);
    }

    void reporter_loop(int interval_s) {
        while (reporter_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(interval_s));
            if (reporter_running.load(std::memory_order_relaxed)) {
                dump_json();
            }
        }
    }

    void start_reporter(int interval_s = 10) {
        reporter_running.store(true, std::memory_order_relaxed);
        reporter_thread = std::thread(&Metrics::reporter_loop, this, interval_s);
    }

    void stop_reporter() {
        reporter_running.store(false, std::memory_order_relaxed);
        if (reporter_thread.joinable()) {
            reporter_thread.join();
        }
        dump_json();
    }
};

} // namespace fatigue
