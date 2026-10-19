/* 契约 §6.1–6.3。CTest 每模式独立进程：runtime 每进程只 Start 一次。
 * fake gate 仅制造受控交错，不作为合规 driver 示例；不修改 runtime/seam。
 */
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <boost/asio/post.hpp>
#include <bbt/coroutine/io/IoExecutor.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace
{
template<class Predicate>
bool WaitUntil(Predicate pred, std::chrono::milliseconds budget = 5000ms)
{
    const auto deadline = Clock::now() + budget;
    while (!pred()) {
        if (Clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

struct Gate
{
    std::mutex mutex;
    std::condition_variable cv;
    bool entered{false}, open{false};
    std::atomic_bool timed_out{false};
    void Pass()
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        cv.notify_all();
        if (!cv.wait_for(lock, 5s, [&] { return open; })) timed_out = true;
    }
    bool WaitEntered()
    {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, 5s, [&] { return entered; });
    }
    void Open()
    {
        { std::lock_guard<std::mutex> lock(mutex); open = true; }
        cv.notify_all();
    }
};
struct OpenOnExit
{
    std::shared_ptr<Gate> gate;
    ~OpenOnExit() { gate->Open(); }
};

void Configure(size_t initial, size_t minimum, size_t maximum)
{
    auto& cfg = *g_bbt_coroutine_config;
    cfg.m_cfg_static_thread_num = initial;
    cfg.m_cfg_processer_min_num = minimum;
    cfg.m_cfg_processer_max_num = maximum;
    cfg.m_cfg_stack_protect = false;
    cfg.m_cfg_worker_stall_warn_ms = 0;
}

void CheckCounts(size_t active, size_t target, size_t retiring, size_t exited)
{
    BOOST_CHECK_EQUAL(g_scheduler->GetProcesserNum(), active);
    BOOST_CHECK_EQUAL(g_scheduler->GetProcesserTargetNum(), target);
    BOOST_CHECK_EQUAL(g_scheduler->GetProcesserRetiringNum(), retiring);
    BOOST_CHECK_EQUAL(g_scheduler->GetProcesserExitedUnjoinedNum(), exited);
}

class Driver final : public IProcesserDriver
{
public:
    explicit Driver(std::shared_ptr<Gate> gate = {}) : m_gate(std::move(gate)) {}
    std::atomic_bool fail{false};
    ProcesserDriverResult OnTick(ProcesserDriverPhase) override
    {
        if (m_gate) m_gate->Pass();
        return fail.load() ? ProcesserDriverResult::Failed : ProcesserDriverResult::Ok;
    }
    ProcesserDriverResult Wait(std::chrono::microseconds timeout) override
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait_for(lock, timeout, [&] { return m_notified; });
        m_notified = false;
        return ProcesserDriverResult::Ok;
    }
    void Wake() noexcept override
    {
        { std::lock_guard<std::mutex> lock(m_mutex); m_notified = true; }
        m_cv.notify_one();
    }
private:
    std::shared_ptr<Gate> m_gate;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_notified{false};
};
struct Binding
{
    ProcesserDriverToken token{};
    bool bound{false};
    Binding(ProcesserId id, const std::shared_ptr<Driver>& driver)
    {
        bound = g_scheduler->RegisterProcesserDriver(id, driver, token) == ProcesserDriverStatus::Ok;
        if (!bound) throw std::runtime_error("scaling test driver bind failed");
    }
    void Release()
    {
        if (bound) {
            const auto rc = g_scheduler->UnregisterProcesserDriver(token);
            bound = false;
            BOOST_CHECK(rc == ProcesserDriverStatus::Ok);
        }
    }
    ~Binding() { if (bound) g_scheduler->UnregisterProcesserDriver(token); }
};

struct TlsReceipt
{
    std::shared_ptr<std::atomic_bool> destroyed;
    std::shared_ptr<Gate> gate;
    ~TlsReceipt()
    {
        if (gate) gate->Pass();
        if (destroyed) destroyed->store(true);
    }
};
thread_local TlsReceipt tls_receipt;

size_t ThreadCount()
{
#if defined(__linux__)
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task")) {
        (void)entry;
        ++count;
    }
    return count;
#else
    return 0;
#endif
}

void Boundaries()
{
    BOOST_CHECK(g_scheduler->SetProcesserNum(2) == ProcesserResizeResult::NotInitialized);
    BOOST_CHECK(g_scheduler->SetProcesserAutoScale(true) == ProcesserResizeResult::NotInitialized);
    CheckCounts(0, 0, 0, 0);
    Configure(2, 2, 4);
    g_bbt_coroutine_config->m_cfg_processer_auto_scale = true;
    g_scheduler->Start(SCHE_START_OPT_SCHE_NO_LOOP);
    BOOST_CHECK(g_scheduler->GetProcesserScalingMode() == ProcesserScalingMode::Auto);
    for (const size_t invalid : {size_t{0}, size_t{1}, size_t{5}, std::numeric_limits<size_t>::max()}) {
        const auto ids = g_scheduler->GetWorkerProcesserIds();
        BOOST_CHECK(g_scheduler->SetProcesserNum(invalid) == ProcesserResizeResult::InvalidTarget);
        CheckCounts(2, 2, 0, 0);
        BOOST_CHECK(g_scheduler->GetProcesserScalingMode() == ProcesserScalingMode::Auto);
        BOOST_CHECK(g_scheduler->GetWorkerProcesserIds() == ids);
    }
    BOOST_CHECK(g_scheduler->SetProcesserNum(2) == ProcesserResizeResult::Ok);
    BOOST_CHECK(g_scheduler->GetProcesserScalingMode() == ProcesserScalingMode::Manual);
    CheckCounts(2, 2, 0, 0);
    BOOST_CHECK(g_scheduler->SetProcesserAutoScale(true) == ProcesserResizeResult::Ok);
    BOOST_CHECK(g_scheduler->GetProcesserScalingMode() == ProcesserScalingMode::Auto);
    BOOST_CHECK(g_scheduler->SetProcesserNum(4) == ProcesserResizeResult::Ok);
    CheckCounts(4, 4, 0, 0);
    BOOST_CHECK(g_scheduler->GetProcesserScalingMode() == ProcesserScalingMode::Manual);
}

/* 两个 worker 都被真实用户代码门占住，故投递必在退役提交之前排队，
 * 退役者当前 Resume 不会强杀，开门后必须 drain 而不能 Resume 下个任务。
 * producer 的前半批完成 -> resize 提交 -> 后半批，固定交会而非概率压测。
 */
void RetireAndReap(bool manual_reaper)
{
    Configure(1, 1, 2);
    const size_t before = ThreadCount();
    g_scheduler->Start(SCHE_START_OPT_SCHE_NO_LOOP);
    auto first = std::make_shared<Gate>();
    auto second = std::make_shared<Gate>();
    OpenOnExit release_first{first}, release_second{second};
    auto completed = std::make_shared<bbt::core::thread::CountDownLatch>(2);
    auto tls_first = std::make_shared<std::atomic_bool>(false);
    auto tls_second = std::make_shared<std::atomic_bool>(false);
    auto id_first = std::make_shared<std::atomic<ProcesserId>>(0);
    auto id_second = std::make_shared<std::atomic<ProcesserId>>(0);
    g_scheduler->RegistCoroutineTask([first, completed, tls_first, id_first] {
        tls_receipt.destroyed = tls_first;
        *id_first = Processer::TryGetLocalProcesser()->GetId();
        first->Pass();
        completed->Down();
    });
    BOOST_REQUIRE(first->WaitEntered());
    BOOST_REQUIRE(g_scheduler->SetProcesserNum(2) == ProcesserResizeResult::Ok);
    g_scheduler->RegistCoroutineTask([second, completed, tls_second, id_second] {
        tls_receipt.destroyed = tls_second;
        *id_second = Processer::TryGetLocalProcesser()->GetId();
        second->Pass();
        completed->Down();
    });
    BOOST_REQUIRE(second->WaitEntered());
    BOOST_REQUIRE(id_first->load() != id_second->load());
#if defined(__linux__)
    BOOST_CHECK_EQUAL(ThreadCount(), before + 2); // 没有隐藏 S/监控线程
#endif
    constexpr size_t tasks = 256;
    struct Batch
    {
        std::vector<std::atomic<int>> hits = std::vector<std::atomic<int>>(tasks);
        std::atomic<ProcesserId> retired{0};
        std::atomic_int ran_on_retired{0};
        bbt::core::thread::CountDownLatch done{tasks};
        Gate halfway;
        std::atomic_bool producer_done{false};
        std::atomic_bool producer_failed{false};
        Batch() { for (auto& hit : hits) hit = 0; }
    };
    auto batch = std::make_shared<Batch>();
    auto post = [batch](size_t i) {
        auto task = [batch, i] {
            if (Processer::TryGetLocalProcesser()->GetId() == batch->retired.load())
                ++batch->ran_on_retired;
            ++batch->hits[i];
            bbtco_yield;
            batch->done.Down();
        };
        g_scheduler->RegistCoroutineTask(task);
    };
    std::thread producer([batch, post] {
        try {
            for (size_t i = 0; i < tasks / 2; ++i) post(i);
            batch->halfway.Pass();
            for (size_t i = tasks / 2; i < tasks; ++i) post(i);
        } catch (...) { batch->producer_failed = true; }
        batch->producer_done = true;
    });
    // 不把 join 留给 fatal 断言展开；producer 的门自身也有 5s 上限。
    const bool halfway = batch->halfway.WaitEntered();
    const auto result = g_scheduler->SetProcesserNum(1);
    const auto active = g_scheduler->GetWorkerProcesserIds();
    const ProcesserId retired = active.front() == id_first->load() ? id_second->load() : id_first->load();
    batch->retired = retired;
    batch->halfway.Open();
    producer.join();
    BOOST_REQUIRE(halfway);
    BOOST_REQUIRE(result == ProcesserResizeResult::Pending);
    BOOST_CHECK(!batch->producer_failed.load());
    BOOST_CHECK(batch->producer_done.load());
    BOOST_CHECK(!batch->halfway.timed_out.load());
    CheckCounts(1, 1, 1, 0);
    BOOST_CHECK(completed->WaitTimeout(0) != 0);
    BOOST_CHECK(!tls_first->load());
    BOOST_CHECK(!tls_second->load());
    BOOST_CHECK_THROW(g_scheduler->RegistCoroutineTaskOn(retired, [] {}), std::invalid_argument);
    ProcesserDriverToken sentinel{123, 456};
    BOOST_CHECK(g_scheduler->RegisterProcesserDriver(retired, std::make_shared<Driver>(), sentinel)
                == ProcesserDriverStatus::InvalidTarget);
    BOOST_CHECK_EQUAL(sentinel.m_processer_id, 123u);
    BOOST_CHECK_EQUAL(sentinel.m_generation, 456u);
    first->Open();
    second->Open();
    BOOST_REQUIRE_EQUAL(completed->WaitTimeout(5000), 0);
    BOOST_REQUIRE_EQUAL(batch->done.WaitTimeout(5000), 0);
    for (const auto& hit : batch->hits) BOOST_CHECK_EQUAL(hit.load(), 1);
    BOOST_CHECK_EQUAL(batch->ran_on_retired.load(), 0);
    BOOST_REQUIRE(WaitUntil([] { return g_scheduler->GetProcesserExitedUnjoinedNum() == 1; }));
    CheckCounts(1, 1, 0, 1);
    const auto tls_retired = retired == id_first->load() ? tls_first : tls_second;
    BOOST_REQUIRE(WaitUntil([tls_retired] { return tls_retired->load(); }));
    // Getter 不推进 reaper；观察窗口跨过实际线程返回，不靠 sleep 判退出。
    for (int i = 0; i < 100; ++i) CheckCounts(1, 1, 0, 1);
    BOOST_REQUIRE(WaitUntil([manual_reaper] {
        if (manual_reaper)
            g_scheduler->SetProcesserNum(1);
        else
            g_scheduler->LoopOnce();
        return g_scheduler->GetProcesserExitedUnjoinedNum() == 0;
    }));
    CheckCounts(1, 1, 0, 0); // join 返回后才从 exited-unjoined 移除
#if defined(__linux__)
    BOOST_REQUIRE(WaitUntil([before] { return ThreadCount() == before + 1; }));
#endif
    BOOST_CHECK(!first->timed_out.load());
    BOOST_CHECK(!second->timed_out.load());
}

void DriverAndParked()
{
    Configure(1, 1, 2);
    g_scheduler->Start();
    auto origin = g_scheduler->GetWorkerProcesserIds().front();
    auto waiter = sync::CoWaiter::Create();
    auto registered = std::make_shared<std::atomic_bool>(false);
    auto done = std::make_shared<bbt::core::thread::CountDownLatch>(1);
    auto resumed = std::make_shared<std::atomic<ProcesserId>>(0);
    auto result = std::make_shared<std::atomic<WaitStatus>>(WaitStatus::RuntimeUnavailable);
    auto destroyed = std::make_shared<std::atomic_bool>(false);
    g_scheduler->RegistCoroutineTask([waiter, registered, done, resumed, result, destroyed] {
        tls_receipt.destroyed = destroyed;
        WaitOptions options;
        options.deadline = Clock::now() + 5s;
        *result = waiter->WaitWithCallback(options, [registered] { *registered = true; return true; });
        *resumed = Processer::TryGetLocalProcesser()->GetId();
        done->Down();
    });
    BOOST_REQUIRE(WaitUntil([registered] { return registered->load(); }));
    // 单 worker 上随后任务完成，证明上一个 Resume 已完成 park disposition。
    auto parked = std::make_shared<bbt::core::thread::CountDownLatch>(1);
    g_scheduler->RegistCoroutineTask([parked] { parked->Down(); });
    BOOST_REQUIRE_EQUAL(parked->WaitTimeout(5000), 0);
    BOOST_REQUIRE(g_scheduler->SetProcesserNum(2) == ProcesserResizeResult::Ok);
    auto ids = g_scheduler->GetWorkerProcesserIds();
    auto survivor = ids.front() == origin ? ids.back() : ids.front();
    Binding keep(survivor, std::make_shared<Driver>());
    Binding pin_origin(origin, std::make_shared<Driver>());
    BOOST_REQUIRE(g_scheduler->SetProcesserAutoScale(true) == ProcesserResizeResult::Ok);
    BOOST_CHECK(g_scheduler->SetProcesserNum(1) == ProcesserResizeResult::DriverBound);
    CheckCounts(2, 2, 0, 0);
    BOOST_CHECK(g_scheduler->GetProcesserScalingMode() == ProcesserScalingMode::Auto);
    pin_origin.Release();
    BOOST_CHECK(g_scheduler->SetProcesserNum(1) == ProcesserResizeResult::Pending);
    BOOST_REQUIRE(WaitUntil([destroyed] {
        return destroyed->load() && g_scheduler->GetProcesserRetiringNum() == 0 &&
               g_scheduler->GetProcesserExitedUnjoinedNum() == 0;
    }));
    BOOST_CHECK(done->WaitTimeout(0) != 0); // 退役未取消/展开 parked 用户栈
    BOOST_CHECK_EQUAL(waiter->Notify(), 0);
    BOOST_REQUIRE_EQUAL(done->WaitTimeout(5000), 0);
    BOOST_CHECK(result->load() == WaitStatus::Completed);
    BOOST_CHECK_EQUAL(resumed->load(), survivor);
    BOOST_CHECK(resumed->load() != origin);
    keep.Release();
}

/* NO_LOOP 控制采样：用时钟越过采样间隔，不用 sleep 猜 worker 的交错。
 * LoopOnce 可能内部多拍；断言返回耗时小于 interval，防止样本计数退化。
 */
void Sample()
{
    const auto due = Clock::now() + 25ms;
    BOOST_REQUIRE(WaitUntil([due] { return Clock::now() >= due; }, 1000ms));
    const auto start = Clock::now();
    g_scheduler->LoopOnce();
    BOOST_REQUIRE(Clock::now() - start < 20ms);
}
void AutoScale(bool queue_signal)
{
    Configure(1, 1, 2);
    auto& cfg = *g_bbt_coroutine_config;
    cfg.m_cfg_processer_scale_interval_ms = 20;
    cfg.m_cfg_processer_scale_up_samples = 2;
    cfg.m_cfg_processer_scale_down_samples = 3;
    cfg.m_cfg_processer_scale_up_queue_watermark = queue_signal ? 1 : std::numeric_limits<size_t>::max();
    cfg.m_cfg_processer_scale_down_busy_percent = 0;
    cfg.m_cfg_processer_auto_scale = true;
    g_scheduler->Start(SCHE_START_OPT_SCHE_NO_LOOP);
    auto gate = std::make_shared<Gate>();
    auto done = std::make_shared<bbt::core::thread::CountDownLatch>(queue_signal ? 64 : 1);
    std::unique_ptr<Binding> binding;
    OpenOnExit release{gate};
    if (queue_signal) {
        binding = std::make_unique<Binding>(g_scheduler->GetWorkerProcesserIds().front(), std::make_shared<Driver>(gate));
        BOOST_REQUIRE(gate->WaitEntered());
        for (int i = 0; i < 64; ++i) g_scheduler->RegistCoroutineTask([done] { done->Down(); });
    } else {
        g_scheduler->RegistCoroutineTask([gate, done] { gate->Pass(); done->Down(); });
        BOOST_REQUIRE(gate->WaitEntered());
    }
    BOOST_REQUIRE(g_scheduler->SetProcesserAutoScale(true) == ProcesserResizeResult::Ok);
    Sample();
    BOOST_CHECK_EQUAL(g_scheduler->GetProcesserNum(), 1u);
    Sample();
    BOOST_CHECK_EQUAL(g_scheduler->GetProcesserNum(), 2u);
    BOOST_CHECK_EQUAL(g_scheduler->GetProcesserTargetNum(), 2u);
    BOOST_CHECK(g_scheduler->GetProcesserScalingMode() == ProcesserScalingMode::Auto);
    BOOST_CHECK_EQUAL(cfg.m_cfg_worker_stall_warn_ms, 0u);
    gate->Open();
    BOOST_REQUIRE_EQUAL(done->WaitTimeout(5000), 0);
    if (binding) binding->Release();
    // 排空后重置采样；前两拍不得缩，连续第三拍才允许 2 -> 1。
    BOOST_REQUIRE(g_scheduler->SetProcesserAutoScale(true) == ProcesserResizeResult::Ok);
    Sample();
    BOOST_CHECK_EQUAL(g_scheduler->GetProcesserNum(), 2u);
    Sample();
    BOOST_CHECK_EQUAL(g_scheduler->GetProcesserNum(), 2u);
    Sample();
    BOOST_CHECK_EQUAL(g_scheduler->GetProcesserNum(), 1u);
    BOOST_CHECK_EQUAL(g_scheduler->GetProcesserTargetNum(), 1u);
    BOOST_REQUIRE(WaitUntil([] { g_scheduler->LoopOnce(); return g_scheduler->GetProcesserRetiringNum() == 0 &&
        g_scheduler->GetProcesserExitedUnjoinedNum() == 0; }));
    BOOST_CHECK(!gate->timed_out.load());
}

void ReapAll()
{
    BOOST_REQUIRE(WaitUntil([] {
        g_scheduler->LoopOnce();
        return g_scheduler->GetProcesserRetiringNum() == 0 &&
               g_scheduler->GetProcesserExitedUnjoinedNum() == 0;
    }));
}

// 两个真实 Resume 占满 P，TLS 析构门与业务门分离；主线程始终能清理两门。
void TlsReaper(bool threaded)
{
    Configure(2, 1, 2);
    g_scheduler->Start(threaded ? SCHE_START_OPT_SCHE_THREAD : SCHE_START_OPT_SCHE_NO_LOOP);
    auto tasks = std::make_shared<Gate>();
    auto tls = std::make_shared<Gate>();
    OpenOnExit release_tasks{tasks}, release_tls{tls};
    auto entered = std::make_shared<std::atomic_int>(0);
    auto done = std::make_shared<std::atomic_int>(0);
    for (int i = 0; i < 2; ++i) g_scheduler->RegistCoroutineTask([tasks, tls, entered, done] {
        tls_receipt.gate = tls;
        ++*entered;
        tasks->Pass();
        ++*done;
    });
    BOOST_REQUIRE(WaitUntil([entered] { return entered->load() == 2; }));
    BOOST_REQUIRE(g_scheduler->SetProcesserNum(1) == ProcesserResizeResult::Pending);
    tasks->Open();
    BOOST_REQUIRE(tls->WaitEntered());
    BOOST_REQUIRE(WaitUntil([done] { return done->load() == 2; }));
    CheckCounts(1, 1, 0, 1);
    auto event = std::make_shared<std::atomic_bool>(false);
    boost::asio::post(io::GetExecutor(), [event] { *event = true; });
    bool progress = false;
    if (threaded) {
        progress = WaitUntil([event] { return event->load(); }, 1000ms);
    } else {
        auto returned = std::make_shared<std::atomic_bool>(false);
        std::thread caller([returned] { g_scheduler->LoopOnce(); *returned = true; });
        progress = WaitUntil([returned, event] { return returned->load() && event->load(); }, 1000ms);
        // 若旧 reaper 阻塞 join，先真实开门再 join caller，不能析构 joinable thread。
        if (!progress) tls->Open();
        caller.join();
    }
    BOOST_CHECK(progress);
    if (progress) CheckCounts(1, 1, 0, 1);
    tls->Open();
    if (threaded) {
        BOOST_REQUIRE(WaitUntil([] { return g_scheduler->GetProcesserExitedUnjoinedNum() == 0; }));
    } else ReapAll();
    CheckCounts(1, 1, 0, 0);
    BOOST_CHECK(!tasks->timed_out.load());
    BOOST_CHECK(!tls->timed_out.load());
}

void SelfRetire()
{
    Configure(2, 1, 2);
    g_scheduler->Start(SCHE_START_OPT_SCHE_NO_LOOP);
    const auto ids = g_scheduler->GetWorkerProcesserIds();
    auto gate = std::make_shared<Gate>();
    Binding keep(ids.back(), std::make_shared<Driver>(gate));
    OpenOnExit release{gate};
    BOOST_REQUIRE(gate->WaitEntered());
    BOOST_REQUIRE(g_scheduler->SetProcesserAutoScale(true) == ProcesserResizeResult::Ok);
    auto result = std::make_shared<std::atomic<ProcesserResizeResult>>(ProcesserResizeResult::Ok);
    auto done = std::make_shared<std::atomic_bool>(false);
    g_scheduler->RegistCoroutineTaskOn(ids.front(), [result, done] {
        *result = g_scheduler->SetProcesserNum(1);
        *done = true;
    });
    BOOST_REQUIRE(WaitUntil([done] { return done->load(); }));
    BOOST_CHECK(result->load() == ProcesserResizeResult::SelfRetire);
    CheckCounts(2, 2, 0, 0);
    BOOST_CHECK(g_scheduler->GetWorkerProcesserIds() == ids);
    BOOST_CHECK(g_scheduler->GetProcesserScalingMode() == ProcesserScalingMode::Auto);
    gate->Open();
    keep.Release();
    BOOST_CHECK(g_scheduler->SetProcesserNum(1) == ProcesserResizeResult::Pending);
    ReapAll();
    BOOST_CHECK(!gate->timed_out.load());
}

void DriverPins(const std::string& state)
{
    Configure(2, 1, 2);
    g_scheduler->Start(SCHE_START_OPT_SCHE_NO_LOOP);
    const auto ids = g_scheduler->GetWorkerProcesserIds();
    Binding keep(ids.back(), std::make_shared<Driver>());
    auto gate = std::make_shared<Gate>();
    auto driver = std::make_shared<Driver>(state == "detaching" ? gate : nullptr);
    driver->fail = state == "failed";
    Binding victim(ids.front(), driver);
    OpenOnExit release{gate};
    if (state == "detaching") BOOST_REQUIRE(gate->WaitEntered());
    const auto expected = state == "failed" ? ProcesserDriverState::Failed : ProcesserDriverState::Bound;
    BOOST_REQUIRE(WaitUntil([&] {
        ProcesserDriverSnapshot snapshot;
        return g_scheduler->QueryProcesserDriver(ids.front(), snapshot) == ProcesserDriverStatus::Ok &&
               snapshot.m_state == expected;
    }));
    std::thread detacher;
    std::atomic<ProcesserDriverStatus> detach_result{ProcesserDriverStatus::StaleToken};
    if (state == "detaching") {
        detacher = std::thread([&] { detach_result = g_scheduler->UnregisterProcesserDriver(victim.token); });
        const bool observed = WaitUntil([&] {
            ProcesserDriverSnapshot snapshot;
            return g_scheduler->QueryProcesserDriver(ids.front(), snapshot) == ProcesserDriverStatus::Ok &&
                   snapshot.m_state == ProcesserDriverState::Detaching;
        });
        // 不在 joinable thread 存活时触发 fatal 断言。
        if (!observed) { gate->Open(); detacher.join(); victim.bound = false; }
        BOOST_REQUIRE(observed);
    }
    const auto mode_rc = g_scheduler->SetProcesserAutoScale(true);
    const auto resize_rc = g_scheduler->SetProcesserNum(1);
    const bool unchanged = g_scheduler->GetWorkerProcesserIds() == ids &&
        g_scheduler->GetProcesserTargetNum() == 2 &&
        g_scheduler->GetProcesserScalingMode() == ProcesserScalingMode::Auto;
    if (detacher.joinable()) {
        gate->Open();
        detacher.join();
        victim.bound = false;
        BOOST_CHECK(detach_result.load() == ProcesserDriverStatus::Ok);
    } else victim.Release();
    CheckCounts(2, 2, 0, 0);
    BOOST_CHECK(mode_rc == ProcesserResizeResult::Ok);
    BOOST_CHECK(resize_rc == ProcesserResizeResult::DriverBound);
    BOOST_CHECK(unchanged);
    BOOST_CHECK(g_scheduler->SetProcesserNum(1) == ProcesserResizeResult::Pending);
    BOOST_CHECK(g_scheduler->GetWorkerProcesserIds() == std::vector<ProcesserId>{ids.back()});
    ReapAll();
    keep.Release();
    BOOST_CHECK(!gate->timed_out.load());
}

void OutstandingLimit()
{
    Configure(1, 1, 2);
    g_scheduler->Start(SCHE_START_OPT_SCHE_NO_LOOP);
    const auto survivor = g_scheduler->GetWorkerProcesserIds().front();
    Binding keep(survivor, std::make_shared<Driver>());
    struct Gates {
        std::vector<std::shared_ptr<Gate>> values;
        ~Gates() { for (auto& gate : values) gate->Open(); }
    } gates;
    auto completed = std::make_shared<std::atomic_int>(0);
    auto occupy = [&](ProcesserId id) {
        auto gate = std::make_shared<Gate>();
        gates.values.push_back(gate);
        g_scheduler->RegistCoroutineTaskOn(id, [gate, completed] { gate->Pass(); ++*completed; });
        return gate->WaitEntered();
    };
    // Survivor 也占住，保证定向任务不会被它 steal，33 个新任务各占唯一新 P。
    BOOST_REQUIRE(occupy(survivor));
    for (int i = 0; i < 33; ++i) {
        BOOST_REQUIRE(g_scheduler->SetProcesserNum(2) == ProcesserResizeResult::Ok);
        const auto ids = g_scheduler->GetWorkerProcesserIds();
        const auto fresh = ids.front() == survivor ? ids.back() : ids.front();
        BOOST_REQUIRE(occupy(fresh));
        if (i < 32) {
            BOOST_REQUIRE(g_scheduler->SetProcesserNum(1) == ProcesserResizeResult::Pending);
            CheckCounts(1, 1, i + 1, 0);
        } else {
            BOOST_REQUIRE(g_scheduler->SetProcesserAutoScale(true) == ProcesserResizeResult::Ok);
            BOOST_CHECK(g_scheduler->SetProcesserNum(1) == ProcesserResizeResult::Busy);
            CheckCounts(2, 2, 32, 0);
            BOOST_CHECK(g_scheduler->GetWorkerProcesserIds() == ids);
            BOOST_CHECK(g_scheduler->GetProcesserScalingMode() == ProcesserScalingMode::Auto);
        }
    }
    for (auto& gate : gates.values) gate->Open();
    BOOST_REQUIRE(WaitUntil([completed] { return completed->load() == 34; }));
    // 关闭 auto 后再推进 LoopOnce，避免本用例引入额外策略请求。
    BOOST_REQUIRE(g_scheduler->SetProcesserAutoScale(false) == ProcesserResizeResult::Ok);
    ReapAll();
    BOOST_CHECK(g_scheduler->SetProcesserNum(1) == ProcesserResizeResult::Pending);
    ReapAll();
    CheckCounts(1, 1, 0, 0);
    for (auto& gate : gates.values) BOOST_CHECK(!gate->timed_out.load());
    keep.Release();
}

void ConcurrentControl()
{
    Configure(1, 1, 3);
    g_scheduler->Start(SCHE_START_OPT_SCHE_NO_LOOP);
    constexpr int count = 512;
    auto hits = std::make_shared<std::vector<std::atomic_int>>(count);
    for (auto& hit : *hits) hit = 0;
    auto completed = std::make_shared<std::atomic_int>(0);
    std::atomic_bool go{false};
    std::atomic_int errors{0};
    std::vector<std::thread> threads;
    for (int c = 0; c < 3; ++c) threads.emplace_back([&, c] {
        while (!go.load()) std::this_thread::yield();
        for (int i = 0; i < 24; ++i) {
            const auto rc = g_scheduler->SetProcesserNum((i + c) % 2 ? 3 : 1);
            if (rc != ProcesserResizeResult::Ok && rc != ProcesserResizeResult::Pending &&
                rc != ProcesserResizeResult::Busy) ++errors;
        }
    });
    threads.emplace_back([&] {
        while (!go.load()) std::this_thread::yield();
        try {
            for (int i = 0; i < count; ++i) g_scheduler->RegistCoroutineTask([hits, completed, i] {
                ++(*hits)[i];
                bbtco_yield;
                ++*completed;
            });
        } catch (...) { ++errors; }
    });
    go = true;
    for (auto& thread : threads) thread.join();
    BOOST_REQUIRE_EQUAL(errors.load(), 0);
    BOOST_REQUIRE(WaitUntil([completed] { return completed->load() == count; }));
    for (auto& hit : *hits) BOOST_CHECK_EQUAL(hit.load(), 1);
    // 最后的手动栅栏在全部调用返回后提交：终态必须唯一且不被旧请求覆盖。
    const auto terminal = g_scheduler->SetProcesserNum(2);
    BOOST_CHECK(terminal == ProcesserResizeResult::Ok || terminal == ProcesserResizeResult::Pending);
    ReapAll();
    CheckCounts(2, 2, 0, 0);
    BOOST_CHECK(g_scheduler->GetProcesserScalingMode() == ProcesserScalingMode::Manual);
}

std::atomic<Gate*> auto_commit_gate{nullptr};

void AutoReverse()
{
    Configure(1, 1, 2);
    auto& cfg = *g_bbt_coroutine_config;
    cfg.m_cfg_processer_auto_scale = true;
    cfg.m_cfg_processer_scale_interval_ms = 20;
    cfg.m_cfg_processer_scale_up_samples = 1;
    cfg.m_cfg_processer_scale_down_samples = 1;
    cfg.m_cfg_processer_scale_up_queue_watermark = std::numeric_limits<size_t>::max();
    cfg.m_cfg_processer_scale_down_busy_percent = 0;
    g_scheduler->Start(SCHE_START_OPT_SCHE_NO_LOOP);
    Binding pin(g_scheduler->GetWorkerProcesserIds().front(), std::make_shared<Driver>());
    auto gate = std::make_shared<Gate>();
    OpenOnExit release{gate};
    auto done = std::make_shared<std::atomic_bool>(false);
    g_scheduler->RegistCoroutineTask([gate, done] { gate->Pass(); *done = true; });
    BOOST_REQUIRE(gate->WaitEntered());
    auto commit = std::make_shared<Gate>();
    OpenOnExit release_commit{commit};
    auto_commit_gate = commit.get();
    const auto due = Clock::now() + 25ms;
    BOOST_REQUIRE(WaitUntil([due] { return Clock::now() >= due; }));
    std::thread sampler([] { g_scheduler->LoopOnce(); });
    const bool auto_locked = commit->WaitEntered();
    std::atomic_bool manual_started{false}, manual_done{false};
    std::atomic<ProcesserResizeResult> manual_rc{ProcesserResizeResult::Busy};
    std::thread controller([&] {
        manual_started = true;
        manual_rc = g_scheduler->SetProcesserNum(1);
        manual_done = true;
    });
    const bool started = WaitUntil([&] { return manual_started.load(); });
    const bool blocked_before_commit = !manual_done.load();
    commit->Open();
    sampler.join();
    controller.join();
    auto_commit_gate = nullptr;
    BOOST_REQUIRE(auto_locked);
    BOOST_CHECK(!commit->timed_out.load());
    BOOST_CHECK(started);
    BOOST_CHECK(blocked_before_commit);
    BOOST_CHECK(manual_rc.load() == ProcesserResizeResult::Pending);
    ReapAll();
    Sample();
    CheckCounts(1, 1, 0, 0); // Auto 持锁准备扩容期间相反手动请求排队；后提交者终态唯一。
    BOOST_REQUIRE(g_scheduler->SetProcesserAutoScale(true) == ProcesserResizeResult::Ok);
    Sample();
    CheckCounts(2, 2, 0, 0);
    gate->Open();
    BOOST_REQUIRE(WaitUntil([done] { return done->load(); }));
    BOOST_REQUIRE(g_scheduler->SetProcesserAutoScale(true) == ProcesserResizeResult::Ok);
    Sample();
    BOOST_CHECK_EQUAL(g_scheduler->GetProcesserTargetNum(), 1u);
    ReapAll();
    CheckCounts(1, 1, 0, 0);
    pin.Release();
    BOOST_CHECK(!gate->timed_out.load());
}

#ifdef BBT_COROUTINE_PROFILE
std::vector<ProcesserId> ProfileIds()
{
    std::string info;
    g_bbt_profiler->ProfileInfo(info);
    std::vector<ProcesserId> ids;
    const std::string marker = "\tPID：";
    for (size_t pos = info.find(marker); pos != std::string::npos; pos = info.find(marker, pos)) {
        pos += marker.size();
        ids.push_back(std::stoull(info.substr(pos)));
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}
void ProfileCycles()
{
    Configure(1, 1, 4);
    g_scheduler->Start(SCHE_START_OPT_SCHE_NO_LOOP);
    std::atomic_bool stop{false};
    std::atomic_int reads{0}, errors{0};
    std::thread reader([&] {
        try { while (!stop.load()) { (void)ProfileIds(); ++reads; } }
        catch (...) { ++errors; }
    });
    bool good = true;
    std::vector<ProcesserId> retired;
    for (int cycle = 0; cycle < 12 && good; ++cycle) {
        good = g_scheduler->SetProcesserNum(4) == ProcesserResizeResult::Ok;
        auto active = g_scheduler->GetWorkerProcesserIds();
        std::sort(active.begin(), active.end());
        good = good && ProfileIds() == active;
        if (good) good = g_scheduler->SetProcesserNum(1) == ProcesserResizeResult::Pending;
        if (good) good = WaitUntil([] {
            g_scheduler->LoopOnce();
            return g_scheduler->GetProcesserRetiringNum() == 0 &&
                   g_scheduler->GetProcesserExitedUnjoinedNum() == 0;
        });
        auto survivor = g_scheduler->GetWorkerProcesserIds();
        for (const auto id : active) if (id != survivor.front()) retired.push_back(id);
        if (good) good = ProfileIds() == survivor;
    }
    stop = true;
    reader.join();
    BOOST_CHECK(good);
    BOOST_CHECK_EQUAL(errors.load(), 0);
    BOOST_CHECK_GT(reads.load(), 0);
    const auto final = ProfileIds();
    for (const auto id : retired) BOOST_CHECK(std::find(final.begin(), final.end(), id) == final.end());
    BOOST_TEST_MESSAGE("profile cycles=12 retired_ids=" << retired.size() << " concurrent_reads=" << reads.load());
}
#endif

// 使用受保护成员的合法 member pointer，不改产品头、不把基类强转成派生对象。
struct ProcesserAccess : Processer {
    static auto Add() { return &ProcesserAccess::AddCoroutineTask; }
};
struct AdmissionState {
    Processer::SPtr target;
    int pause_point{0};
    std::shared_ptr<Gate> pause = std::make_shared<Gate>();
    std::atomic_bool drain_entered{false}, drained{false};
};
std::atomic<AdmissionState*> admission_state{nullptr};
thread_local bool admission_producer = false;

void Admission(int point)
{
    Configure(1, 1, 2);
    g_scheduler->Start(SCHE_START_OPT_SCHE_NO_LOOP);
    const auto survivor = g_scheduler->GetWorkerProcesserIds().front();
    Binding keep(survivor, std::make_shared<Driver>());
    auto survivor_gate = std::make_shared<Gate>();
    auto victim_gate = std::make_shared<Gate>();
    OpenOnExit release_survivor{survivor_gate}, release_victim{victim_gate};
    g_scheduler->RegistCoroutineTask([survivor_gate] { survivor_gate->Pass(); });
    BOOST_REQUIRE(survivor_gate->WaitEntered());
    BOOST_REQUIRE(g_scheduler->SetProcesserNum(2) == ProcesserResizeResult::Ok);
    auto captured = std::make_shared<Processer::SPtr>();
    g_scheduler->RegistCoroutineTask([captured, victim_gate] {
        *captured = Processer::TryGetLocalProcesser();
        victim_gate->Pass();
    });
    BOOST_REQUIRE(victim_gate->WaitEntered());
    AdmissionState state;
    state.target = *captured; // 保留退役之前的真实 P 强引用（旧快照）。
    state.pause_point = point;
    OpenOnExit release_pause{state.pause};
    auto hits = std::make_shared<std::atomic_int>(0);
    auto ran_on = std::make_shared<std::atomic<ProcesserId>>(0);
    auto* coroutine = Coroutine::Create(g_bbt_coroutine_config->m_cfg_stack_size, [hits, ran_on] {
        *ran_on = Processer::TryGetLocalProcesser()->GetId();
        ++*hits;
    }, false, "admission regression");
    admission_state = &state;
    std::atomic_bool accepted{false};
    std::thread producer([&] {
        admission_producer = true;
        accepted = (state.target.get()->*ProcesserAccess::Add())(CO_PRIORITY_NORMAL, coroutine);
        admission_producer = false;
    });
    const bool paused = state.pause->WaitEntered();
    const auto resize = g_scheduler->SetProcesserNum(1);
    victim_gate->Open();
    const bool at_drain = WaitUntil([&] { return state.drain_entered.load(); });
    bool before_resume = false;
    if (point == 1) {
        // 首次 admission 通过但尚未登记；允许 drain 完成，然后复查必须拒绝。
        before_resume = WaitUntil([&] { return state.drained.load(); });
    } else {
        // 已登记并通过复查、尚未 enqueue；drain 必须等待 producer 槽位。
        before_resume = !WaitUntil([&] { return state.drained.load(); }, 100ms) &&
                        g_scheduler->GetProcesserRetiringNum() == 1;
    }
    state.pause->Open();
    producer.join();
    const bool drained = WaitUntil([&] { return state.drained.load(); });
    admission_state = nullptr;
    if (!accepted.load()) delete coroutine; // false 保留调用者所有权。
    survivor_gate->Open();
    BOOST_REQUIRE(paused);
    BOOST_REQUIRE(resize == ProcesserResizeResult::Pending);
    BOOST_CHECK(at_drain);
    BOOST_CHECK(before_resume);
    BOOST_CHECK(drained);
    BOOST_CHECK_EQUAL(accepted.load(), point == 2);
    if (point == 2) {
        BOOST_REQUIRE(WaitUntil([hits] { return hits->load() == 1; }));
        BOOST_CHECK_EQUAL(ran_on->load(), survivor);
    } else BOOST_CHECK_EQUAL(hits->load(), 0);
    ReapAll();
    BOOST_CHECK_EQUAL(hits->load(), point == 2 ? 1 : 0);
    BOOST_CHECK(!state.pause->timed_out.load());
    BOOST_CHECK(!survivor_gate->timed_out.load());
    BOOST_CHECK(!victim_gate->timed_out.load());
    keep.Release();
}
} // namespace

namespace
{
void RebalanceShortBacklog()
{
    Configure(1, 1, 4);
    g_scheduler->Start(SCHE_START_OPT_SCHE_NO_LOOP);
    const auto primary = g_scheduler->GetWorkerProcesserIds().front();
    auto first = std::make_shared<Gate>();
    auto current = std::make_shared<Gate>();
    OpenOnExit release_first{first};
    OpenOnExit release_current{current};
    constexpr size_t tasks = 1200;
    auto hits = std::make_shared<std::vector<std::atomic_uint>>(tasks);
    for (auto& hit : *hits) hit.store(0);
    auto completed = std::make_shared<std::atomic_size_t>(0);
    auto on_new = std::make_shared<std::atomic_size_t>(0);

    g_scheduler->RegistCoroutineTaskOn(primary, [first] { first->Pass(); });
    BOOST_REQUIRE(first->WaitEntered());
    g_scheduler->RegistCoroutineTaskOn(primary, [current] { current->Pass(); });
    for (size_t i = 0; i < tasks; ++i) {
        g_scheduler->RegistCoroutineTaskOn(primary, [primary, hits, completed, on_new, i] {
            const auto end = Clock::now() + 200us;
            while (Clock::now() < end) std::atomic_signal_fence(std::memory_order_seq_cst);
            if (Processer::TryGetLocalProcesser()->GetId() != primary) ++*on_new;
            (*hits)[i].fetch_add(1);
            completed->fetch_add(1, std::memory_order_release);
        });
    }
    // 刷新原 worker 的本次 Resume 起点，不能靠排队准备耗时越过旧偷取阈值。
    first->Open();
    BOOST_REQUIRE(current->WaitEntered());
    BOOST_REQUIRE(g_scheduler->SetProcesserNum(4) == ProcesserResizeResult::Ok);
    current->Open();
    BOOST_REQUIRE(WaitUntil([completed] { return completed->load(std::memory_order_acquire) >= tasks; }));
    BOOST_CHECK_EQUAL(completed->load(), tasks);
    BOOST_CHECK_GT(on_new->load(), 0); // 容量增加必须能消费原有短任务积压，而非仅增加线程。
    for (const auto& hit : *hits) BOOST_CHECK_EQUAL(hit.load(), 1);
    BOOST_CHECK(!first->timed_out.load());
    BOOST_CHECK(!current->timed_out.load());
    BOOST_CHECK(g_scheduler->SetProcesserNum(1) == ProcesserResizeResult::Pending);
    ReapAll();
}
} // namespace

// CMake 的测试专用 Processer.cc 副本仅加入观察点，条件/原子序/锁均原样保留。
void ScalingAutoCommitProbe()
{
    if (auto* gate = auto_commit_gate.load()) gate->Pass();
}

void ScalingAdmissionProbe(Processer* processer, int point)
{
    auto* state = admission_state.load();
    if (!state || state->target.get() != processer) return;
    if (point == 3) state->drain_entered = true;
    else if (point == 4) state->drained = true;
    else if (admission_producer && state->pause_point == point) state->pause->Pass();
}

BOOST_AUTO_TEST_CASE(scaling)
{
    const char* selected = std::getenv("BBT_SCALING_MODE");
    const std::string mode = selected ? selected : "boundaries";
    if (mode == "boundaries") Boundaries();
    else if (mode == "noloop_loop_reaper") RetireAndReap(false);
    else if (mode == "noloop_manual_reaper") RetireAndReap(true);
    else if (mode == "driver_parked") DriverAndParked();
    else if (mode == "auto_busy") AutoScale(false);
    else if (mode == "auto_queue") AutoScale(true);
    else if (mode == "tls_thread") TlsReaper(true);
    else if (mode == "tls_noloop") TlsReaper(false);
    else if (mode == "self_retire") SelfRetire();
    else if (mode == "pin_bound") DriverPins("bound");
    else if (mode == "pin_failed") DriverPins("failed");
    else if (mode == "pin_detaching") DriverPins("detaching");
    else if (mode == "outstanding_limit") OutstandingLimit();
    else if (mode == "concurrent_control") ConcurrentControl();
    else if (mode == "auto_reverse") AutoReverse();
    else if (mode == "admission_late") Admission(1);
    else if (mode == "admission_inflight") Admission(2);
    else if (mode == "rebalance_short_backlog") RebalanceShortBacklog();
#ifdef BBT_COROUTINE_PROFILE
    else if (mode == "profile_cycles") ProfileCycles();
#endif
    else BOOST_FAIL("unknown BBT_SCALING_MODE: " + mode);
}
