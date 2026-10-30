/* 可重复短跑入口；同一进程只 Start 一次。fixed-old 必须显式按旧头/库编译，
 * 不能把候选 runtime 关闭 auto 后冒充旧实现。CSV 为观测值，不作性能门槛判定。
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;
using Clock = std::chrono::steady_clock;

namespace
{
#ifdef BBT_SCALING_LEGACY_RUNTIME
// 防止把候选头/库仅改标签后冒充 fixed-old；实际旧 DSO 身份仍须调用方记录。
template<class T, class = void> struct HasResize : std::false_type {};
template<class T> struct HasResize<T, std::void_t<decltype(std::declval<T&>().SetProcesserNum(size_t{}))>>
    : std::true_type {};
static_assert(!HasResize<Scheduler>::value, "fixed-old must use old runtime headers without SetProcesserNum");
#endif
struct Options
{
    std::string mode{"dynamic"}, output;
    size_t workers{4}, tasks{2000}, runs{3}, warmup{1}, task_us{30};
};
size_t Number(const std::string& text, bool zero_allowed = false)
{
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("expected unsigned decimal integer: " + text);
    size_t used = 0;
    const auto value = std::stoull(text, &used);
    if (used != text.size() || value > std::numeric_limits<size_t>::max() || (!zero_allowed && value == 0))
        throw std::invalid_argument("integer out of range: " + text);
    return static_cast<size_t>(value);
}
Options Parse(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            std::cout << "--mode fixed-old|fixed-candidate|dynamic --workers N --tasks N --runs N "
                         "--warmup N [--task-us N] [--output path]\n"
                         "Default: dynamic, workers=4, tasks=2000, runs=3, warmup=1, task-us=30.\n"
                         "fixed-old requires a BBT_SCALING_LEGACY_RUNTIME build linked to the old runtime.\n";
            std::exit(0);
        }
        if (++i == argc) throw std::invalid_argument("missing value for " + arg);
        const std::string value = argv[i];
        if (arg == "--mode") options.mode = value;
        else if (arg == "--output") options.output = value;
        else if (arg == "--workers") options.workers = Number(value);
        else if (arg == "--tasks") options.tasks = Number(value);
        else if (arg == "--runs") options.runs = Number(value);
        else if (arg == "--warmup") options.warmup = Number(value, true);
        else if (arg == "--task-us") options.task_us = Number(value, true);
        else throw std::invalid_argument("unknown option: " + arg);
    }
    if (options.mode != "fixed-old" && options.mode != "fixed-candidate" && options.mode != "dynamic")
        throw std::invalid_argument("invalid mode: " + options.mode);
    if (options.workers > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        options.tasks > 10000000 || options.runs > 10000 || options.warmup > 10000 || options.task_us > 1000000)
        throw std::invalid_argument("budget exceeds workers=INT_MAX/tasks=10000000/runs=10000/warmup=10000/task-us=1000000");
#ifdef BBT_SCALING_LEGACY_RUNTIME
    if (options.mode != "fixed-old") throw std::invalid_argument("legacy build only accepts fixed-old");
#else
    if (options.mode == "fixed-old")
        throw std::invalid_argument("fixed-old requires rebuilding this source against verified old headers/library with BBT_SCALING_LEGACY_RUNTIME=ON");
#endif
    return options;
}
struct Resources
{
    size_t threads{0}, rss_kib{0};
};
Resources ReadResources()
{
    Resources resources;
#if defined(__linux__)
    std::ifstream input("/proc/self/status");
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string key;
        size_t value = 0;
        if (fields >> key >> value) {
            if (key == "Threads:") resources.threads = value;
            else if (key == "VmRSS:") resources.rss_kib = value;
        }
    }
#endif
    return resources;
}
std::string ValueOrNA(size_t value) { return value ? std::to_string(value) : "NA"; }
struct RunState
{
    explicit RunState(size_t tasks) : hits(tasks)
    {
        for (auto& hit : hits) hit.store(0, std::memory_order_relaxed);
    }
    std::vector<std::atomic_uint> hits;
    std::atomic<size_t> completed{0};
    std::atomic_bool abort{false};
};

void Run(const Options& options, size_t run, bool warmup, std::ostream& output)
{
#ifndef BBT_SCALING_LEGACY_RUNTIME
    if (options.mode == "dynamic") {
        if (g_scheduler->SetProcesserNum(1) != ProcesserResizeResult::Ok)
            throw std::runtime_error("reset to one worker failed");
        if (g_scheduler->SetProcesserAutoScale(true) != ProcesserResizeResult::Ok)
            throw std::runtime_error("enable auto failed");
    }
#endif
    auto state = std::make_shared<RunState>(options.tasks);
#ifndef BBT_SCALING_LEGACY_RUNTIME
    size_t peak_workers = g_scheduler->GetProcesserNum();
#endif
    auto resources = ReadResources();
    size_t peak_threads = resources.threads, peak_rss = resources.rss_kib;
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::seconds(30);
    auto sample = [&] {
#ifndef BBT_SCALING_LEGACY_RUNTIME
        peak_workers = std::max(peak_workers, g_scheduler->GetProcesserNum());
#endif
        const auto current = ReadResources();
        peak_threads = std::max(peak_threads, current.threads);
        peak_rss = std::max(peak_rss, current.rss_kib);
    };
    for (size_t i = 0; i < options.tasks; ++i) {
        g_scheduler->RegistCoroutineTask([state, i, task_us = options.task_us] {
            const auto until = Clock::now() + std::chrono::microseconds(task_us);
            while (!state->abort.load(std::memory_order_relaxed) && Clock::now() < until) {
                // 故意不 yield：短 CPU burst，busy 与 backlog 均可被 auto 观测。
                std::atomic_signal_fence(std::memory_order_seq_cst);
            }
            state->hits[i].fetch_add(1, std::memory_order_relaxed);
            state->completed.fetch_add(1, std::memory_order_release);
        });
        if (i % 64 == 0) sample();
        if (Clock::now() >= deadline) { state->abort = true; throw std::runtime_error("submission exceeded 30s"); }
    }
    while (state->completed.load(std::memory_order_acquire) < options.tasks) {
        sample();
        if (Clock::now() >= deadline) { state->abort = true; throw std::runtime_error("drain exceeded 30s"); }
        std::this_thread::yield();
    }
    const auto drained = Clock::now();
    if (state->completed.load(std::memory_order_acquire) != options.tasks)
        throw std::runtime_error("task completion count mismatch");
    for (const auto& hit : state->hits)
        if (hit.load(std::memory_order_relaxed) != 1)
            throw std::runtime_error("task lost or executed more than once");
    sample();
    std::string idle_ms{"NA"};
#ifndef BBT_SCALING_LEGACY_RUNTIME
    if (options.mode == "dynamic") {
        const auto idle_deadline = drained + std::chrono::seconds(5);
        while (g_scheduler->GetProcesserNum() != 1 || g_scheduler->GetProcesserTargetNum() != 1 ||
               g_scheduler->GetProcesserRetiringNum() != 0 || g_scheduler->GetProcesserExitedUnjoinedNum() != 0) {
            sample();
            if (Clock::now() >= idle_deadline) throw std::runtime_error("idle fallback/join exceeded 5s");
            std::this_thread::yield();
        }
        idle_ms = std::to_string(std::chrono::duration<double, std::milli>(Clock::now() - drained).count());
    }
#endif
    const double elapsed = std::chrono::duration<double>(drained - start).count();
    resources = ReadResources();
#ifdef BBT_SCALING_LEGACY_RUNTIME
    const std::string worker_peak = "NA"; // 旧 API 无 active 查询，不把配置值伪装成采样峰值。
#else
    const std::string worker_peak = std::to_string(peak_workers);
#endif
    output << options.mode << ',' << (warmup ? "warmup" : "run") << ',' << run << ',' << options.workers << ','
           << options.tasks << ',' << options.task_us << ',' << std::fixed << std::setprecision(3)
           << elapsed * 1000 << ',' << options.tasks / elapsed << ',' << worker_peak << ',' << idle_ms << ','
           << ValueOrNA(peak_threads) << ',' << ValueOrNA(peak_rss) << ','
           << ValueOrNA(resources.threads) << ',' << ValueOrNA(resources.rss_kib) << '\n';
    output.flush();
}
} // namespace

int main(int argc, char** argv)
{
    try {
        const auto options = Parse(argc, argv);
        std::ofstream file;
        if (!options.output.empty()) {
            file.open(options.output, std::ios::out | std::ios::trunc);
            if (!file) throw std::runtime_error("cannot open output: " + options.output);
        }
        auto& cfg = *g_bbt_coroutine_config;
        cfg.m_cfg_static_thread_num = options.mode == "dynamic" ? 1 : options.workers;
        cfg.m_cfg_max_coroutine = options.tasks + 128;
        cfg.m_cfg_stack_protect = false;
#ifndef BBT_SCALING_LEGACY_RUNTIME
        cfg.m_cfg_processer_min_num = options.mode == "dynamic" ? 1 : options.workers;
        cfg.m_cfg_processer_max_num = options.workers;
        cfg.m_cfg_processer_auto_scale = options.mode == "dynamic";
        cfg.m_cfg_processer_scale_interval_ms = 5;
        cfg.m_cfg_processer_scale_up_samples = 1;
        cfg.m_cfg_processer_scale_down_samples = 3;
        cfg.m_cfg_processer_scale_step = 1;
        cfg.m_cfg_processer_scale_up_busy_percent = 80;
        cfg.m_cfg_processer_scale_down_busy_percent = 20;
        cfg.m_cfg_processer_scale_up_queue_watermark = 1;
        cfg.m_cfg_processer_scale_down_queue_watermark = 0;
        cfg.m_cfg_worker_stall_warn_ms = 0;
#endif
        g_scheduler->Start();
        auto& output = file.is_open() ? static_cast<std::ostream&>(file) : std::cout;
        output << "# sampling=main-thread; peak fields are sampled, not exact maxima; drain includes submission; "
                  "stack_protect=false; auto=";
        if (options.mode == "dynamic")
            output << "interval_ms=5/up_samples=1/down_samples=3/step=1/busy=80:20/queue=1:0";
        else
            output << "disabled";
        output << '\n'
               << "mode,phase,run,workers_cap,tasks,task_us,drain_ms,throughput_tasks_s,peak_workers_sampled,"
                  "idle_fallback_join_ms,linux_peak_threads_sampled,linux_peak_rss_kib_sampled,linux_idle_threads,linux_idle_rss_kib\n";
        for (size_t i = 0; i < options.warmup; ++i) Run(options, i + 1, true, output);
        for (size_t i = 0; i < options.runs; ++i) Run(options, i + 1, false, output);
        if (!output) throw std::runtime_error("CSV write failed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark_processer_scaling: " << error.what() << '\n';
        return 1;
    }
}
