/*
 * B1 processer driver adapter 测试（契约 §11）。
 *
 * 每个用例独立进程（runtime 只 Start 一次），经环境变量 BBT_PD_MODE 选择：
 *   api_boundaries   tick_sequence   wait_wakeup   unregister_waits
 *   self_detach      fail_tick_ret   fail_tick_throw
 *   fail_wait_ret    fail_wait_throw  reentrancy    steal_migrate
 *   no_driver_directed   query_destruct_release
 *
 * 约定（fake driver 不是合规性能示例，只验证 runtime 语义）：
 *  - 所有 gate/回调配置在 Register 之前一次性写入 shared FakeState，
 *    绑定存续期不改 plain 成员；
 *  - 异步回调改写的状态全部 atomic/mutex 保护，fake state 由 shared_ptr
 *    活到同步解绑与任务 join 之后；
 *  - 门统一 entered/open 两段握手，不用 sleep 猜测停在 gate；
 *  - 不在 callback 内抛 Boost 断言异常，断言尽量主线程执行。
 */
#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <functional>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/syntax/SyntaxMacro.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/interface/IProcesserDriver.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>
#include <bbt/coroutine/sync/Chan.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

namespace bbt::coroutine::detail
{
/* Boost.Test 打印支持：scoped enum 无内建 operator<< */
static std::ostream& operator<<(std::ostream& os, ProcesserDriverStatus v)
{ return os << static_cast<int>(v); }
static std::ostream& operator<<(std::ostream& os, ProcesserDriverState v)
{ return os << static_cast<int>(v); }
static std::ostream& operator<<(std::ostream& os, ProcesserDriverFailure v)
{ return os << static_cast<int>(v); }
[[maybe_unused]] static std::ostream& operator<<(std::ostream& os, const ProcesserDriverToken& v)
{ return os << "{pid=" << v.m_processer_id << ",gen=" << v.m_generation << "}"; }
}

namespace
{

void EnsureRuntime(size_t workers = 2)
{
    static std::once_flag once;
    std::call_once(once, [workers]() {
        g_bbt_coroutine_config->m_cfg_static_thread_num = workers;
        g_bbt_coroutine_config->m_cfg_stack_protect = false;
        Scheduler::GetInstance()->Start();
    });
}

/* fatal 断言先同步收尾，再让 Boost 展开栈；引用捕获仍在其作用域内。
 * 所有门、异步控制调用、绑定和业务任务均登记，不靠进程退出掩盖悬挂。 */
struct TestControl
{
    std::mutex m;
    std::vector<std::function<void()>> releases;
    std::vector<ProcesserDriverToken> bindings;
    std::atomic<int> tasks{0};
    static TestControl& Get() { static TestControl c; return c; }
    void Release() {
        std::vector<std::function<void()>> pending;
        { std::lock_guard<std::mutex> l(m); pending = releases; }
        for (auto& f : pending) if (f) f();
    }
    void Cleanup();
};
int require_visits = 0;
bool RequireResult(bool result)
{
    ++require_visits;
    const char* fail = std::getenv("BBT_PD_FAIL_REQUIRE_AT");
    return result && (!fail || std::atoi(fail) != require_visits);
}
struct AssertionCleanup
{
    bool failed;
    ~AssertionCleanup() { if (failed) TestControl::Get().Cleanup(); }
};
#define PD_REQUIRE(expr) do { const bool ok = RequireResult(static_cast<bool>(expr)); \
    AssertionCleanup cleanup{!ok}; BOOST_REQUIRE_MESSAGE(ok, #expr); } while (false)

/* entered/open 两段握手门：回调进入时发 entered，停在 open 放行前 */
struct Gate
{
    std::mutex m;
    std::condition_variable cv;
    bool entered{false};
    bool open{false};
    size_t release_id;
    Gate() {
        auto& c = TestControl::Get();
        std::lock_guard<std::mutex> l(c.m);
        release_id = c.releases.size();
        c.releases.push_back([this]{ Open(); });
    }
    ~Gate() {
        Open();
        auto& c = TestControl::Get();
        std::lock_guard<std::mutex> l(c.m);
        c.releases[release_id] = {};
    }
    Gate(const Gate&) = delete;
    Gate& operator=(const Gate&) = delete;

    void Wait()
    {
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [&]{ return open; });
    }
    void PassThrough()
    {
        std::unique_lock<std::mutex> l(m);
        entered = true;
        cv.notify_all();
        cv.wait(l, [&]{ return open; });
    }
    bool WaitEntered(int ms = 5000)
    {
        std::unique_lock<std::mutex> l(m);
        return cv.wait_for(l, std::chrono::milliseconds(ms), [&]{ return entered; });
    }
    void Open() { { std::lock_guard<std::mutex> l(m); open = true; } cv.notify_all(); }
};

struct CleanupAction
{
    size_t index;
    explicit CleanupAction(std::function<void()> f) {
        auto& c = TestControl::Get(); std::lock_guard<std::mutex> l(c.m);
        index = c.releases.size(); c.releases.push_back(std::move(f));
    }
    ~CleanupAction() {
        auto& c = TestControl::Get(); std::lock_guard<std::mutex> l(c.m);
        c.releases[index] = {};
    }
};

/* 只用于测试控制线程，析构先开所有门，后 join/get。 */
struct JoiningThread : std::thread
{
    using std::thread::thread;
    ~JoiningThread() { if (joinable()) { TestControl::Get().Release(); join(); } }
};
template<class T> struct JoiningFuture : std::future<T>
{
    explicit JoiningFuture(std::future<T>&& f) : std::future<T>(std::move(f)) {}
    ~JoiningFuture() { if (this->valid()) { TestControl::Get().Release(); this->wait(); } }
};
template<class F> auto Async(std::launch policy, F&& f)
{
    using T = decltype(f());
    return JoiningFuture<T>(std::async(policy, std::forward<F>(f)));
}
ProcesserDriverStatus Register(Scheduler& s, ProcesserId p,
    const std::shared_ptr<IProcesserDriver>& d, ProcesserDriverToken& t)
{
    auto rc = s.RegisterProcesserDriver(p, d, t);
    if (rc == ProcesserDriverStatus::Ok) {
        auto& c = TestControl::Get();
        std::lock_guard<std::mutex> l(c.m);
        c.bindings.push_back(t);
    }
    return rc;
}
template<class F> auto TrackedTask(F f)
{
    ++TestControl::Get().tasks;
    return [f = std::move(f)]() mutable {
        struct Done { ~Done() { --TestControl::Get().tasks; } } done;
        f();
    };
}
template<class F> void Post(Scheduler& s, F f) { s.RegistCoroutineTask(TrackedTask(std::move(f))); }
template<class F> void PostOn(Scheduler& s, ProcesserId p, F f) {
    s.RegistCoroutineTaskOn(p, TrackedTask(std::move(f)));
}
void TestControl::Cleanup()
{
    Release();
    std::vector<ProcesserDriverToken> tokens;
    { std::lock_guard<std::mutex> l(m); tokens.swap(bindings); }
    for (auto t : tokens) Scheduler::GetInstance()->UnregisterProcesserDriver(t);
    // 门均已打开；等待引用捕获的业务任务完全退出，才允许断言展开。
    while (tasks.load() != 0) std::this_thread::yield();
    std::cout << "PD_CLEANUP tasks=0 bindings_released=" << tokens.size() << std::endl;
}

/* fake 资源：mutex 保护的真实状态（fake handle + pending completion
 * 队列），全部 driver callback 经 RAII guard 接入；cleanup 只允许
 * 同步解绑确认（MarkDetached）且 active==0 后由控制线程单 owner 推进。
 * fake 不是 Asio/真实关闭，只验证 runtime 交接语义可达。 */
struct FakeResource
{
    enum Kind { TickK = 0, WaitK = 1, WakeK = 2 };

    std::mutex              m;
    std::thread::id         owner{};              // cleanup owner
    std::thread::id         pump_tid{};           // Tick/Wait 唯一 pump 线程
    bool                    pump_conflict{false};
    bool                    detached{false};      // Unregister Ok 后标记
    bool                    cleanup_started{false};
    int                     violations{0};
    int                     active{0};
    int                     access_seq{0};
    int                     visits[3]{0, 0, 0};
    /* fake 资源内容：两个 handle 与 pending completion 队列 */
    std::vector<int>        handles{1, 2};        // fake handle id
    std::vector<int>        closed_handles;       // 已关闭但未 drain
    struct Completion { int id; std::function<void()> callback; };
    std::vector<Completion> pending;              // 真正待执行的 cancel completion
    std::atomic<int> completion_effect{0};
    std::vector<int>        completed;            // 确实执行过的 completion id

    void Enter(Kind k)
    {
        std::lock_guard<std::mutex> l(m);
        ++active; ++access_seq; ++visits[k];
        if (k != WakeK) {
            auto tid = std::this_thread::get_id();
            if (pump_tid == std::thread::id()) pump_tid = tid;
            else if (pump_tid != tid) pump_conflict = true;
        }
        if (detached || cleanup_started || (owner != std::thread::id() &&
            owner != std::this_thread::get_id()))
            ++violations;   // callback 与 cleanup 重叠 / owner 越域
    }
    void Leave(Kind k)
    {
        std::lock_guard<std::mutex> l(m);
        --active;
    }
    /* 只允许 Unregister Ok（detached）且 active==0；拒绝不改任何状态 */
    bool TryBeginCleanup(std::thread::id self)
    {
        std::lock_guard<std::mutex> l(m);
        if (!detached || active != 0 || cleanup_started)
            return false;
        owner = self;
        cleanup_started = true;
        if (self == pump_tid) ++violations;   // cleanup owner 不得是 pump
        return true;
    }
    void MarkDetached()
    {
        std::lock_guard<std::mutex> l(m);
        detached = true;
    }
    /* 有界 CloseOne：关一个 handle 并入队其 cancel completion */
    bool CloseOne(std::thread::id self)
    {
        std::lock_guard<std::mutex> l(m);
        if (!cleanup_started || owner != self || active != 0 ||
            handles.empty())
            { ++violations; return false; }
        int hid = handles.back();
        handles.pop_back();
        closed_handles.push_back(hid);
        pending.push_back({hid, [this, hid] { completion_effect.fetch_add(hid); }});
        return true;
    }
    /* 有界 DrainOne：真正执行并移除一个 pending completion */
    bool DrainOne(std::thread::id self)
    {
        std::lock_guard<std::mutex> l(m);
        if (!cleanup_started || owner != self || active != 0 ||
            pending.empty())
            { ++violations; return false; }
        auto completion = std::move(pending.back());
        pending.pop_back();
        completion.callback();          // 执行 CloseOne 已排队的取消 callback
        completed.push_back(completion.id);
        return true;
    }
    /* 锁内快照读取 */
    void Snapshot(int& h, int& p, int& c, int& a, int& seq, int& viol)
    {
        std::lock_guard<std::mutex> l(m);
        h = (int)handles.size(); p = (int)pending.size();
        c = (int)completed.size(); a = active; seq = access_seq;
        viol = violations;
    }
    int AccessSeq()
    {
        std::lock_guard<std::mutex> l(m);
        return access_seq;
    }
};

struct FakeState
{
    /* 观测（callback 写 / 主线程读，全 atomic 或 mutex 保护） */
    std::atomic<int> before{0}, after{0}, wakes{0}, waits{0};
    std::atomic<int> bad_ctx{0};          // Tick/Wait 中观察到业务协程
    std::atomic<ProcesserId> tick_pid{0};
    std::thread::id tick_tid;             // 仅 worker 写 / 解绑后主线程读
    std::atomic<int> active_callbacks{0}; // Tick/Wait/Wake 在途计数
    std::atomic<int> wake_exited_by_timeout{0};

    /* 相位记录（tick_sequence）：worker 写，解绑后主线程读 */
    std::mutex phase_m;
    std::vector<int> phases;              // 1=Before 2=After

    /* Wait 持久通知 */
    std::mutex wait_m;
    std::condition_variable wait_cv;
    bool notified{false};
    /* early-wake 编号：每 Wait 一次 id；wait_exits 在 wait_m 内记录
     * 每次退出来源；selected_wait_id 由 CAS 选中唯一受控 Wait。 */
    std::atomic<int> wait_id_seq{0};
    std::atomic<int> selected_wait_id{0};
    std::atomic<int> wait_exit_notified{0}, wait_exit_timeout{0};
    std::map<int, int> wait_exits;      // id -> 1 notified / 0 timeout
    /* 终止 Wake 退出握手：query_on_wake 分类后，终止 Wake 尾部自增 */
    std::atomic<int> terminal_exited{0};
    std::condition_variable terminal_cv;
    std::mutex terminal_m;

    /* ---- 以下为绑定前一次性配置，绑定存续期不改 ---- */
    ProcesserDriverResult tick_result{ProcesserDriverResult::Ok};
    ProcesserDriverResult wait_result{ProcesserDriverResult::Ok};
    bool tick_throw{false};
    bool wait_throw{false};
    const char* throw_msg{nullptr};
    Gate* tick_gate{nullptr};
    /* 开关用 atomic（绑定后可变）：register 时的首次 Wake 先于 arm，
     * 不会被 gate 卡住；armed 之后所有 gate 生效。failure_enabled 与
     * arm_gates 分离：确保 callback 先进 gate、再允许失败生效。 */
    std::atomic_bool arm_gates{false};
    std::atomic_bool failure_enabled{false};
    Gate* wait_gate{nullptr};
    Gate* wake_gate{nullptr};             // 普通 Wake 门
    Gate* terminal_gate{nullptr};         // 终止 Wake 专用门（区分 leader）
    ProcesserId self_pid{0};              // terminal 识别用
    bool query_on_wake{false};            // Wake 内 Query 区分普通/终止
    bool record_phase{false};
    std::shared_ptr<FakeResource> res;    // 可选：接入全部 callback
    std::function<void()> on_before;      // 只首轮 before 触发语义由内部 once 控制
    std::function<void()> on_wake;
    std::function<void()> on_after;
    Gate* pump_gate{nullptr};
    std::atomic_bool hold_pump{false};
};

class FakeDriver final : public IProcesserDriver
{
public:
    explicit FakeDriver(std::shared_ptr<FakeState> st) : m_st(std::move(st)) {}

    /* callback 全程 RAII 访问计数（含异常退出），并记录 pump tid */
    struct ResGuard {
        std::shared_ptr<FakeResource> r;
        std::atomic<int>& counter;
        ResGuard(std::atomic<int>& count, const std::shared_ptr<FakeResource>& res,
                 FakeResource::Kind kind)
            : r(res), counter(count), k(kind)
        {
            ++counter;
            if (r) r->Enter(k);
        }
        ~ResGuard() { if (r) r->Leave(k); --counter; }
        FakeResource::Kind k;
    };

    ProcesserDriverResult OnTick(ProcesserDriverPhase phase) override
    {
        ResGuard guard(m_st->active_callbacks, m_st->res, FakeResource::TickK);
        auto proc = Processer::TryGetLocalProcesser();
        m_st->tick_pid.store(proc ? proc->GetId() : 0, std::memory_order_release);
        m_st->tick_tid = std::this_thread::get_id();
        if (m_st->pump_gate && m_st->hold_pump.load()) m_st->pump_gate->PassThrough();
        if (GetLocalCoroutineId() != 0)
            m_st->bad_ctx++;
        if (phase == ProcesserDriverPhase::BeforeSchedule) {
            m_st->before++;
            if (m_st->on_before) m_st->on_before();
        } else {
            m_st->after++;
            if (m_st->on_after) m_st->on_after();
        }
        if (m_st->record_phase) {
            std::lock_guard<std::mutex> l(m_st->phase_m);
            m_st->phases.push_back(phase == ProcesserDriverPhase::BeforeSchedule ? 1 : 2);
        }
        if (m_st->tick_gate && m_st->arm_gates.load(std::memory_order_acquire)) m_st->tick_gate->PassThrough();
        /* 失败只在 gate 之后、且 failure_enabled 显式开启才生效：
         * 保证 worker 确定先进入 gate，再产生故障。 */
        if (!m_st->failure_enabled.load(std::memory_order_acquire))
            return ProcesserDriverResult::Ok;
        if (m_st->tick_throw) throw std::runtime_error{m_st->throw_msg ? m_st->throw_msg : "tick"};
        return m_st->tick_result;
    }

    ProcesserDriverResult Wait(std::chrono::microseconds timeout) override
    {
        ResGuard guard(m_st->active_callbacks, m_st->res, FakeResource::WaitK);
        m_st->waits++;
        auto proc = Processer::TryGetLocalProcesser();
        if (proc == nullptr || GetLocalCoroutineId() != 0)
            m_st->bad_ctx++;
        const int wait_id = ++m_st->wait_id_seq;
        if (m_st->wait_gate && m_st->arm_gates.load(std::memory_order_acquire)) {
            /* 唯一受控 Wait：CAS 选中并在 wait_m 内清残留 latch，
             * 使控制线程的 Wake 置位成为本次唯一唤醒来源 */
            int expect = 0;
            if (m_st->selected_wait_id.compare_exchange_strong(expect, wait_id)) {
                std::lock_guard<std::mutex> l(m_st->wait_m);
                m_st->notified = false;
            }
            m_st->wait_gate->PassThrough();
        }  // entered 握手在实际阻塞前
        bool by_notify = false;
        {
            std::unique_lock<std::mutex> l(m_st->wait_m);
            if (m_st->wait_cv.wait_for(l, timeout, [&]{ return m_st->notified; }))
                by_notify = true;
            m_st->notified = false;
        }
        {
            std::lock_guard<std::mutex> l(m_st->wait_m);
            m_st->wait_exits[wait_id] = by_notify ? 1 : 0;
        }
        if (by_notify) m_st->wait_exit_notified++; else m_st->wait_exit_timeout++;
        if (!m_st->failure_enabled.load(std::memory_order_acquire))
            return ProcesserDriverResult::Ok;
        if (m_st->wait_throw) throw std::runtime_error{m_st->throw_msg ? m_st->throw_msg : "wait"};
        return m_st->wait_result;
    }

    void Wake() noexcept override
    {
        bool was_terminal = false;
        struct TerminalExit {
            FakeState& st;
            bool& terminal;
            ~TerminalExit() {
                if (!terminal) return;
                std::lock_guard<std::mutex> l(st.terminal_m);
                ++st.terminal_exited;
                st.terminal_cv.notify_all();
            }
        } exit{*m_st, was_terminal}; // 最后析构：整个 callback 资源/计数先退出
        ResGuard guard(m_st->active_callbacks, m_st->res, FakeResource::WakeK);
        m_st->wakes++;
        if (m_st->on_wake) m_st->on_wake();
        /* query_on_wake：区分终止 Wake（state==Detaching）走独立门 */
        if (m_st->query_on_wake) {
            ProcesserDriverSnapshot snap;
            auto st = Scheduler::GetInstance()->QueryProcesserDriver(m_st->self_pid, snap);
            if (st == ProcesserDriverStatus::Ok && snap.m_state == ProcesserDriverState::Detaching) {
                was_terminal = true;
                if (m_st->terminal_gate) m_st->terminal_gate->PassThrough();
            } else if (m_st->wake_gate && m_st->arm_gates.load(std::memory_order_acquire)) {
                m_st->wake_gate->PassThrough();
            }
        } else if (m_st->wake_gate && m_st->arm_gates.load(std::memory_order_acquire)) {
            m_st->wake_gate->PassThrough();
        }
        {
            std::lock_guard<std::mutex> l(m_st->wait_m);
            m_st->notified = true;
        }
        m_st->wait_cv.notify_all();
    }
private:
    std::shared_ptr<FakeState> m_st;
};

bool WaitUntil(const std::function<bool()>& pred, int ms = 5000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

bool LatchWait(bbt::core::thread::CountDownLatch& latch, int ms = 5000)
{
    return latch.WaitTimeout(ms) == 0;
}

/* worker gate 只在真实 Tick 内阻塞：绑定前配置，确认实际 P 后才投任务。 */
struct WorkerFence
{
    std::shared_ptr<FakeState> st = std::make_shared<FakeState>();
    Gate gate;
    ProcesserDriverToken token{};
    explicit WorkerFence(ProcesserId pid) {
        st->pump_gate = &gate;
        st->hold_pump = true;
        if (Register(*Scheduler::GetInstance(), pid, std::make_shared<FakeDriver>(st), token)
            != ProcesserDriverStatus::Ok) throw std::runtime_error("worker fence bind");
        PD_REQUIRE(gate.WaitEntered());
        PD_REQUIRE(st->tick_pid.load() == pid);
    }
    void Open() { st->hold_pump = false; gate.Open(); }
    ~WorkerFence() {
        Open();
        Scheduler::GetInstance()->UnregisterProcesserDriver(token);
    }
};

/* 只读 probe：复制 waiter 当前登记事件，锁外读状态。不修改生产状态。 */
struct CoWaiterProbe : sync::CoWaiter
{
    int SnapshotEventStatus()
    {
        std::shared_ptr<detail::CoPollEvent> ev;
        {
            std::lock_guard<std::mutex> l(m_notify_mutex);
            ev = m_co_event;
        }
        return ev ? static_cast<int>(ev->GetStatus()) : -1;
    }
};

struct ChanProbe : sync::Chan<int, 4>
{
    std::shared_ptr<CoWaiterProbe> reader = std::make_shared<CoWaiterProbe>();
    ChanProbe() { m_enable_read_cond = reader; }
};

} // namespace

// 仅专用测试可执行的 Processer 副本插入两个观察点；产品库无 hook。
#ifdef BBT_PD_CONTROLLED_PROBE
struct PDProbeControl
{
    std::mutex m;
    std::function<void()> joiner, continued;
    static PDProbeControl& Get() { static auto* p = new PDProbeControl; return *p; }
};
void PDSetProbe(bool joiner, std::function<void()> callback)
{
    auto& p = PDProbeControl::Get();
    std::lock_guard<std::mutex> l(p.m);
    (joiner ? p.joiner : p.continued) = std::move(callback);
}
void PDTestJoinerProbe() {
    // joiner gate 不持探针 mutex，允许 main 配置 continue 观察；清除在 join 后。
    std::function<void()> f;
    { auto& p = PDProbeControl::Get(); std::lock_guard<std::mutex> l(p.m); f = p.joiner; }
    if (f) f();
}
void PDTestContinueProbe() {
    auto& p = PDProbeControl::Get(); std::lock_guard<std::mutex> l(p.m);
    if (p.continued) p.continued();
}
#endif

BOOST_AUTO_TEST_SUITE(ProcesserDriver)

/* ---------- 1. 枚举/绑定/查询/解绑边界 ---------- */
void CaseApiBoundaries()
{
    auto& sche = *Scheduler::GetInstance();

    /* 未初始化：空枚举，绑定/查询/解绑一律 NotInitialized，输出不被改 */
    BOOST_TEST(sche.GetWorkerProcesserIds().empty());
    ProcesserDriverToken tok{123, 456};
    ProcesserDriverSnapshot snap;
    snap.m_state = ProcesserDriverState::Bound;   // 哨兵：失败不得改输出
    snap.m_failure = ProcesserDriverFailure::Wait;
    BOOST_TEST(Register(sche, 1, std::make_shared<FakeDriver>(std::make_shared<FakeState>()), tok)
               == ProcesserDriverStatus::NotInitialized);
    BOOST_TEST(tok.m_processer_id == 123u);   // 失败不改输出 token
    BOOST_TEST(tok.m_generation == 456u);
    BOOST_TEST(sche.QueryProcesserDriver(1, snap) == ProcesserDriverStatus::NotInitialized);
    BOOST_TEST(snap.m_state == ProcesserDriverState::Bound);
    BOOST_TEST(sche.UnregisterProcesserDriver({1, 1}) == ProcesserDriverStatus::NotInitialized);

    EnsureRuntime(2);
    auto ids = sche.GetWorkerProcesserIds();
    BOOST_TEST(ids.size() == 2u);
    const ProcesserId target = ids.front();

    /* 非法 id、主线程惰性 P、空 driver */
    BOOST_TEST(Register(sche, 999999, std::make_shared<FakeDriver>(std::make_shared<FakeState>()), tok)
               == ProcesserDriverStatus::InvalidTarget);
    BOOST_TEST(Register(sche, Processer::GetLocalProcesser()->GetId(),
                                            std::make_shared<FakeDriver>(std::make_shared<FakeState>()), tok)
               == ProcesserDriverStatus::InvalidTarget);
    BOOST_TEST(Register(sche, target, nullptr, tok)
               == ProcesserDriverStatus::InvalidDriver);
    BOOST_TEST(tok.m_generation == 456u);   // 失败不改输出

    auto st1 = std::make_shared<FakeState>();
    auto drv1 = std::make_shared<FakeDriver>(st1);
    PD_REQUIRE(Register(sche, target, drv1, tok) == ProcesserDriverStatus::Ok);
    BOOST_TEST(tok.m_processer_id == target);
    BOOST_TEST(tok.m_generation != 0u);
    const auto gen1 = tok.m_generation;

    BOOST_TEST(Register(sche, target, std::make_shared<FakeDriver>(std::make_shared<FakeState>()), tok)
               == ProcesserDriverStatus::Occupied);
    BOOST_TEST(tok.m_generation == gen1);  // 失败不改输出

    BOOST_TEST(sche.QueryProcesserDriver(target, snap) == ProcesserDriverStatus::Ok);
    BOOST_TEST(snap.m_state == ProcesserDriverState::Bound);
    BOOST_TEST(snap.m_token.m_generation == gen1);
    snap.m_state = ProcesserDriverState::Failed;
    BOOST_TEST(sche.QueryProcesserDriver(999999, snap) == ProcesserDriverStatus::InvalidTarget);
    BOOST_TEST(snap.m_state == ProcesserDriverState::Failed); // 失败不改输出

    /* 解绑：伪 token / 旧 token / 正确 token / 重绑 generation 变化 */
    BOOST_TEST(sche.UnregisterProcesserDriver({999999, 1}) == ProcesserDriverStatus::StaleToken);
    BOOST_TEST(sche.UnregisterProcesserDriver({target, gen1 + 100}) == ProcesserDriverStatus::StaleToken);
    PD_REQUIRE(sche.UnregisterProcesserDriver(tok) == ProcesserDriverStatus::Ok);
    BOOST_TEST(sche.UnregisterProcesserDriver(tok) == ProcesserDriverStatus::StaleToken);
    BOOST_TEST(sche.QueryProcesserDriver(target, snap) == ProcesserDriverStatus::Ok);
    BOOST_TEST(snap.m_state == ProcesserDriverState::Unbound);
    BOOST_TEST(snap.m_token.m_processer_id == target);
    BOOST_TEST(snap.m_token.m_generation == 0u);

    auto drv2 = std::make_shared<FakeDriver>(std::make_shared<FakeState>());
    ProcesserDriverToken tok2{};
    PD_REQUIRE(Register(sche, target, drv2, tok2) == ProcesserDriverStatus::Ok);
    BOOST_TEST(tok2.m_generation != gen1);
    BOOST_TEST(sche.UnregisterProcesserDriver(tok) == ProcesserDriverStatus::StaleToken); // 前代不解新绑
    BOOST_TEST(sche.UnregisterProcesserDriver(tok2) == ProcesserDriverStatus::Ok);

    /* 定向注册：未登记目标抛 invalid_argument */
    BOOST_CHECK_THROW(sche.RegistCoroutineTaskOn(999999, []{}), std::invalid_argument);
    BOOST_CHECK_THROW(sche.RegistCoroutineTaskOn(Processer::GetLocalProcesser()->GetId(), []{}),
                      std::invalid_argument);
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<ProcesserId> ran_pid{0};
    PostOn(sche, target, [&]{
        auto p = Processer::TryGetLocalProcesser();
        ran_pid = p ? p->GetId() : 0;
        done.Down();
    });
    BOOST_TEST(LatchWait(done));
}

/* ---------- 2. Tick 相位序列与 continue 覆盖 ---------- */
void CaseTickSequence()
{
    auto& sche = *Scheduler::GetInstance();
    /* 测试启动前压预算 1us：批量投递必然走 any_dequeued&&local>0 的
     * continue 公平轮次；每 task yield 回 global，覆盖真实恢复路径。 */
    g_bbt_coroutine_config->m_cfg_processer_priority_runtime_budget_us = {1, 1, 1, 1};
    EnsureRuntime(1);
    auto ids = sche.GetWorkerProcesserIds();
    PD_REQUIRE(ids.size() == 1u);

    auto st = std::make_shared<FakeState>();
    st->record_phase = true;
    std::atomic<int> ran{0}, outstanding{0}, wait_while_busy{0};
    std::atomic_bool all_tasks_bracketed{false};
    st->on_after = [&] {
        if (ran.load() == 3 && outstanding.load() == 0) all_tasks_bracketed = true;
    };
    st->on_before = [&]{
        /* 首轮 before 一次批量投递同 P 三个任务；配置在绑定前写入，
         * 这里只用 atomic once 保证一次性 */
        static std::atomic_flag posted = ATOMIC_FLAG_INIT;
        if (!posted.test_and_set()) {
            for (int i = 0; i < 3; ++i) {
                outstanding++;
                PostOn(sche, ids[0], [&]{
                    /* 每轮 yield 回全局队列：未完成前始终 runnable；
                     * 此刻 worker 不应进入 driver Wait。 */
                    for (int j = 0; j < 3; ++j) {
                        { std::lock_guard<std::mutex> l(st->phase_m); st->phases.push_back(3); }
                        bbtco_yield;
                    }
                    { std::lock_guard<std::mutex> l(st->phase_m); st->phases.push_back(3); }
                    ran++;
                    outstanding--;
                });
            }
        }
    };
    /* Wait 进入时若仍有 outstanding 任务记错误（FullDriver 内读
     * 同一个 shared atomic）。 */
    class FullDriver final : public IProcesserDriver {
    public:
        FullDriver(std::shared_ptr<FakeState> s, std::atomic<int>* out, std::atomic<int>* err)
            : m_st(std::move(s)), m_out(out), m_err(err) {}
        ProcesserDriverResult OnTick(ProcesserDriverPhase phase) override {
            auto proc = Processer::TryGetLocalProcesser();
            m_st->tick_pid.store(proc ? proc->GetId() : 0, std::memory_order_release);
            if (proc == nullptr || proc->GetId() != m_expected_pid ||
                GetLocalCoroutineId() != 0)
                m_st->bad_ctx++;
            { std::lock_guard<std::mutex> l(m_st->phase_m);
              m_st->phases.push_back(phase == ProcesserDriverPhase::BeforeSchedule ? 1 : 2); }
            if (phase == ProcesserDriverPhase::BeforeSchedule) {
                m_st->before++;
                if (m_st->on_before) m_st->on_before();
            } else {
                m_st->after++;
                if (m_st->on_after) m_st->on_after();
            }
            return ProcesserDriverResult::Ok;
        }
        ProcesserDriverResult Wait(std::chrono::microseconds t) override {
            auto proc = Processer::TryGetLocalProcesser();
            if (proc == nullptr || proc->GetId() != m_expected_pid ||
                GetLocalCoroutineId() != 0)
                m_st->bad_ctx++;
            if (m_out->load() > 0) (*m_err)++;   // outstanding>0 不该进 Wait
            m_st->waits++;
            std::unique_lock<std::mutex> l(m_st->wait_m);
            m_st->wait_cv.wait_for(l, t, [&]{ return m_st->notified; });
            m_st->notified = false;
            return ProcesserDriverResult::Ok;
        }
        void Wake() noexcept override {
            m_st->wakes++;
            { std::lock_guard<std::mutex> l(m_st->wait_m); m_st->notified = true; }
            m_st->wait_cv.notify_all();
        }
        ProcesserId m_expected_pid{0};
    private:
        std::shared_ptr<FakeState> m_st;
        std::atomic<int>* m_out;
        std::atomic<int>* m_err;
    };

#ifdef BBT_PD_CONTROLLED_PROBE
    PDSetProbe(false, [st] {
        std::lock_guard<std::mutex> l(st->phase_m); st->phases.push_back(4);
    });
#endif
    auto drv = std::make_shared<FullDriver>(st, &outstanding, &wait_while_busy);
    drv->m_expected_pid = ids[0];
    ProcesserDriverToken tok{};
    PD_REQUIRE(Register(sche, ids[0], drv, tok) == ProcesserDriverStatus::Ok);

    BOOST_TEST(WaitUntil([&]{ return ran.load() == 3; }));
    /* 给公平轮次留窗口再解绑，期间 outstanding 应已归零 */
    PD_REQUIRE(WaitUntil([&]{ return all_tasks_bracketed.load(); }));
    BOOST_TEST(st->before.load() >= 3);
    BOOST_TEST(sche.UnregisterProcesserDriver(tok) == ProcesserDriverStatus::Ok);

    /* 解绑后主线程读相位序列：允许动态绑定首次为 After、末尾遗留
     * Before，其余严格 B/A 交错，且至少 3 轮成对（continue 覆盖） */
    std::vector<int> ph;
    { std::lock_guard<std::mutex> l(st->phase_m); ph = st->phases; }
    int pairs = 0, tasks = 0, continues = 0;
    int previous_phase = 0;
    for (size_t i = 0; i < ph.size(); ++i) {
        if (ph[i] == 3) {
            ++tasks;
            BOOST_TEST(previous_phase == 1); // 业务事件必须在 B 后、A 前
            auto next_phase = std::find_if(ph.begin() + i + 1, ph.end(), [](int e) { return e == 1 || e == 2; });
            PD_REQUIRE(next_phase != ph.end());
            BOOST_TEST(*next_phase == 2);
        } else if (ph[i] == 4) {
            ++continues;
            BOOST_TEST(previous_phase == 2); // 实际 continue 必经 A
        } else {
            if (previous_phase != 0) BOOST_TEST(ph[i] != previous_phase); // 含首对
            if (previous_phase == 1 && ph[i] == 2) ++pairs;
            previous_phase = ph[i];
        }
    }
    BOOST_TEST(pairs >= 3);
    BOOST_TEST(tasks == 12);
#ifdef BBT_PD_CONTROLLED_PROBE
    PDSetProbe(false, {});
    BOOST_TEST(continues > 0);
#endif
    BOOST_TEST(wait_while_busy.load() == 0);
    BOOST_TEST(st->bad_ctx.load() == 0);
    BOOST_TEST(st->tick_pid.load() == ids[0]);
}

/* ---------- 3. 早 Wake 持久化（真实握手，不靠超时兜底） ---------- */
void CaseWaitWakeup()
{
    auto& sche = *Scheduler::GetInstance();
    EnsureRuntime(1);
    auto ids = sche.GetWorkerProcesserIds();
    auto st = std::make_shared<FakeState>();
    Gate entry_gate;            // Wait 入口停在实际 predicate 阻塞前
    st->wait_gate = &entry_gate;
    auto drv = std::make_shared<FakeDriver>(st);

    ProcesserDriverToken tok{};
    PD_REQUIRE(Register(sche, ids[0], drv, tok) == ProcesserDriverStatus::Ok);
    st->arm_gates.store(true, std::memory_order_release);

    /* 选中一次 Wait：worker 进入 PassThrough 前清掉注册 Wake 残留的
     * notified latch（FakeDriver 的 Wait 在 gate 前于 wait_m 内清）；
     * 控制线程确认 entered → 发任务（同步 Wake 完成持久置位）→ 开门；
     * 该 id 的退出必须是 Notified，且任务执行。 */
    PD_REQUIRE(entry_gate.WaitEntered());
    bbt::core::thread::CountDownLatch task_done{1};
    const int sel = st->selected_wait_id.load();
    PD_REQUIRE(sel != 0);
    Post(sche, [&]{ task_done.Down(); });   // publish→Wake 持久置位
    entry_gate.Open();
    BOOST_TEST(WaitUntil([&]{
        std::lock_guard<std::mutex> l(st->wait_m);
        auto it = st->wait_exits.find(sel);
        return it != st->wait_exits.end();
    }));
    {
        std::lock_guard<std::mutex> l(st->wait_m);
        BOOST_TEST(st->wait_exits.at(sel) == 1);          // Notified 退出
    }
    BOOST_TEST(LatchWait(task_done));

    /* 解绑唤醒 worker 回 CV 路径，普通任务仍可执行 */
    BOOST_TEST(sche.UnregisterProcesserDriver(tok) == ProcesserDriverStatus::Ok);
    bbt::core::thread::CountDownLatch done2{1};
    Post(sche, [&]{ done2.Down(); });
    BOOST_TEST(LatchWait(done2));
}

/* ---------- 4. Unregister 等在途 Tick/Wait/普通 Wake ---------- */
void CaseUnregisterWaits()
{
    auto& sche = *Scheduler::GetInstance();
    EnsureRuntime(1);
    auto ids = sche.GetWorkerProcesserIds();

    /* (a) 在途 Wait：entered 握手 + future 未 ready + 放行后 Ok */
    {
        auto st = std::make_shared<FakeState>();
        Gate wait_gate;
        st->wait_gate = &wait_gate;
        auto drv = std::make_shared<FakeDriver>(st);
        ProcesserDriverToken tok{};
        PD_REQUIRE(Register(sche, ids[0], drv, tok) == ProcesserDriverStatus::Ok);
        st->arm_gates.store(true, std::memory_order_release);   // 绑定后开闸
        PD_REQUIRE(wait_gate.WaitEntered());
        auto fut = Async(std::launch::async,
                              [&]{ return sche.UnregisterProcesserDriver(tok); });
        BOOST_TEST(static_cast<bool>(
            fut.wait_for(std::chrono::milliseconds(300)) != std::future_status::ready));
        wait_gate.Open();
        PD_REQUIRE(static_cast<bool>(
            fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready));
        BOOST_TEST(fut.get() == ProcesserDriverStatus::Ok);
        drv.reset(); st.reset();
    }

    /* (b) 在途 Tick */
    {
        auto st = std::make_shared<FakeState>();
        Gate tick_gate;
        st->tick_gate = &tick_gate;
        auto drv = std::make_shared<FakeDriver>(st);
        ProcesserDriverToken tok{};
        PD_REQUIRE(Register(sche, ids[0], drv, tok) == ProcesserDriverStatus::Ok);
        st->arm_gates.store(true, std::memory_order_release);
        PD_REQUIRE(tick_gate.WaitEntered());
        auto fut = Async(std::launch::async,
                              [&]{ return sche.UnregisterProcesserDriver(tok); });
        BOOST_TEST(static_cast<bool>(
            fut.wait_for(std::chrono::milliseconds(300)) != std::future_status::ready));
        tick_gate.Open();
        PD_REQUIRE(static_cast<bool>(
            fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready));

        BOOST_TEST(fut.get() == ProcesserDriverStatus::Ok);
        drv.reset(); st.reset();
    }

    /* (c) 在途普通 Wake（独立门）+ 终止 Wake 也经独立门确认。
     * 普通 Wake 被 wake_gate 阻塞期间 Unregister 不得返回；放开普通
     * Wake 但 terminal_gate 仍关时，终止 Wake 卡住 → 仍不得返回。
     * 证明普通与终止在途分别被等待。 */
    {
        auto st = std::make_shared<FakeState>();
        Gate wake_gate, terminal_gate;
        st->wake_gate = &wake_gate;
        st->terminal_gate = &terminal_gate;
        st->query_on_wake = true;
        st->self_pid = ids[0];
        auto drv = std::make_shared<FakeDriver>(st);
        ProcesserDriverToken tok{};
        PD_REQUIRE(Register(sche, ids[0], drv, tok) == ProcesserDriverStatus::Ok);
        st->arm_gates.store(true, std::memory_order_release);

        /* 制造一次外线程普通 Wake：helper 线程发任务触发广播，
         * Wake 在投递线程同步执行并停在 wake_gate（不能由主线程投——
         * 主线程会卡在 Wake 里无法继续推进测试）。 */
        JoiningThread waker([&]{ PostOn(sche, ids[0], []{}); });

        PD_REQUIRE(wake_gate.WaitEntered());   // 普通 Wake 已进入并阻塞

        auto fut = Async(std::launch::async,
                              [&]{ return sche.UnregisterProcesserDriver(tok); });
        BOOST_TEST(static_cast<bool>(
            fut.wait_for(std::chrono::milliseconds(300)) != std::future_status::ready));
        /* I2 顺序：普通 Wake 仍在独立 gate 内阻塞时，先让终止 Wake
         * 进入并完全退出（terminal_gate 开），此时只剩普通 Wake 的
         * lease 未退——detach 必须仍未 ready；再开普通门才完成。
         * 证明普通 Wake 的在途 lease 被独立等待，不被终止 Wake 掩盖。 */
        PD_REQUIRE(terminal_gate.WaitEntered());
        terminal_gate.Open();
        /* 终止 Wake 退出后仍因普通 Wake 阻塞：leader 在等 inflight */
        PD_REQUIRE(WaitUntil([&]{ return st->terminal_exited.load() == 1; }));
        BOOST_TEST(static_cast<bool>(
            fut.wait_for(std::chrono::milliseconds(300)) != std::future_status::ready));
        wake_gate.Open();
        PD_REQUIRE(static_cast<bool>(
            fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready));
        BOOST_TEST(fut.get() == ProcesserDriverStatus::Ok);
        waker.join();
        drv.reset(); st.reset();
    }
}

/* ---------- 5. self-detach / 嵌套 / ABA ---------- */
void CaseSelfDetach()
{
    auto& sche = *Scheduler::GetInstance();
    EnsureRuntime(2);
    auto ids = sche.GetWorkerProcesserIds();
    PD_REQUIRE(ids.size() == 2u);

    /* (a) 终止 Wake 内嵌套同代 Unregister → SelfDetach（TLS adopt 覆盖）。
     * 旧实现（终止 Wake 无 TLS 记录）会在此等待自身——真实死锁。 */
    {
        auto st = std::make_shared<FakeState>();
        auto drv = std::make_shared<FakeDriver>(st);
        std::atomic<ProcesserDriverStatus> nested{ProcesserDriverStatus::Ok};
        std::atomic_bool observed_detaching{false};
        struct TokenHolder { ProcesserDriverToken t{}; std::atomic_bool ready{false}; };
        auto holder = std::make_shared<TokenHolder>();
        /* 配置先于绑定；绑定时的首次 Wake 早 ready 置位，安全跳过；
         * 终止 Wake 到达时 ready 已置位。 */
        st->on_wake = [&, holder]{
            if (!holder->ready.load(std::memory_order_acquire))
                return;
            ProcesserDriverSnapshot snap;
            if (sche.QueryProcesserDriver(ids[0], snap) == ProcesserDriverStatus::Ok &&
                snap.m_state == ProcesserDriverState::Detaching)
            {
                observed_detaching = true;
                nested = sche.UnregisterProcesserDriver(holder->t); // 嵌套同代
            }
        };
        ProcesserDriverToken tokA{};
        PD_REQUIRE(Register(sche, ids[0], drv, tokA) == ProcesserDriverStatus::Ok);
        holder->t = tokA;
        holder->ready.store(true, std::memory_order_release);
        BOOST_TEST(sche.UnregisterProcesserDriver(tokA) == ProcesserDriverStatus::Ok);
        BOOST_TEST(observed_detaching.load());
        BOOST_TEST(nested.load() == ProcesserDriverStatus::SelfDetach);
        drv.reset(); st.reset();
    }

    /* (b) Tick（目标 worker 线程 + lease 双重拒绝）与受管业务协程
     * 的 self-detach。回调全部绑定前配置，内部用 atomic once。 */
    auto st = std::make_shared<FakeState>();
    auto drv = std::make_shared<FakeDriver>(st);
    std::atomic<ProcesserDriverStatus> tick_ret{ProcesserDriverStatus::Ok};
    auto tick_once = std::make_shared<std::atomic_bool>(true);
    struct TokenHolder2 { ProcesserDriverToken t{}; std::atomic_bool ready{false}; };
    auto holder2 = std::make_shared<TokenHolder2>();
    st->on_before = [&, tick_once, holder2]{
        if (!holder2->ready.load(std::memory_order_acquire)) return;
        if (!tick_once->exchange(false)) return;
        tick_ret = sche.UnregisterProcesserDriver(holder2->t);
    };
    ProcesserDriverToken tok{};
    PD_REQUIRE(Register(sche, ids[0], drv, tok) == ProcesserDriverStatus::Ok);
    holder2->t = tok;
    holder2->ready.store(true, std::memory_order_release);
    BOOST_TEST(WaitUntil([&]{ return !tick_once->load(); }));
    BOOST_TEST(tick_ret.load() == ProcesserDriverStatus::SelfDetach);

    std::atomic<ProcesserDriverStatus> co_ret{ProcesserDriverStatus::Ok};
    bbt::core::thread::CountDownLatch co_done{1};
    Post(sche, [&]{ co_ret = sche.UnregisterProcesserDriver(tok); co_done.Down(); });
    BOOST_TEST(LatchWait(co_done));
    BOOST_TEST(co_ret.load() == ProcesserDriverStatus::SelfDetach);

    // A.Wake -> 真实定向投递 -> B.Wake -> Unregister(A)：A 不是栈顶 lease。
    BOOST_TEST(sche.UnregisterProcesserDriver(tok) == ProcesserDriverStatus::Ok);
    auto nestedA = std::make_shared<FakeState>();
    auto nestedB = std::make_shared<FakeState>();
    ProcesserDriverToken tokA{}, tokB{};
    std::atomic_bool nested_enabled{false};
    static thread_local int depth = 0;
    std::atomic<int> nested_calls{0};
    std::atomic<ProcesserDriverStatus> wake_self{ProcesserDriverStatus::Ok};
    nestedA->on_wake = [&] {
        if (!nested_enabled.load()) return;
        depth = 1;
        PostOn(sche, ids[1], []{});
        depth = 0;
    };
    nestedB->on_wake = [&] {
        if (depth != 1) return;
        ++nested_calls;
        wake_self = sche.UnregisterProcesserDriver(tokA);
    };
    PD_REQUIRE(Register(sche, ids[0], std::make_shared<FakeDriver>(nestedA), tokA)
               == ProcesserDriverStatus::Ok);
    PD_REQUIRE(Register(sche, ids[1], std::make_shared<FakeDriver>(nestedB), tokB)
               == ProcesserDriverStatus::Ok);
    nested_enabled = true;
    PostOn(sche, ids[0], []{});
    nested_enabled = false;
    BOOST_TEST(nested_calls.load() == 1);
    BOOST_TEST(wake_self.load() == ProcesserDriverStatus::SelfDetach);
    ProcesserDriverSnapshot nested_snap;
    BOOST_TEST(sche.QueryProcesserDriver(ids[0], nested_snap) == ProcesserDriverStatus::Ok);
    BOOST_TEST(nested_snap.m_state == ProcesserDriverState::Bound); // 拒绝前无副作用
    BOOST_TEST(sche.UnregisterProcesserDriver(tokA) == ProcesserDriverStatus::Ok);

#ifdef BBT_PD_CONTROLLED_PROBE
    // 唯一 seam 在 joiner 已捕获旧 binding、且确定非 leader 后。
    // 停住旧 joiner，让 leader 完成、新代绑定，再恢复旧操作。
    Gate terminal, captured_joiner;
    auto old = std::make_shared<FakeState>();
    old->query_on_wake = true;
    old->self_pid = ids[0];
    old->terminal_gate = &terminal;
    ProcesserDriverToken old_token{}, new_token{};
    PD_REQUIRE(Register(sche, ids[0], std::make_shared<FakeDriver>(old), old_token)
               == ProcesserDriverStatus::Ok);
    auto leader = Async(std::launch::async, [&]{ return sche.UnregisterProcesserDriver(old_token); });
    PD_REQUIRE(terminal.WaitEntered());
    PDSetProbe(true, [&]{ captured_joiner.PassThrough(); });
    auto joiner = Async(std::launch::async, [&]{ return sche.UnregisterProcesserDriver(old_token); });
    PD_REQUIRE(captured_joiner.WaitEntered());
    terminal.Open();
    BOOST_TEST(leader.get() == ProcesserDriverStatus::Ok);
    PD_REQUIRE(Register(sche, ids[0], std::make_shared<FakeDriver>(std::make_shared<FakeState>()), new_token)
               == ProcesserDriverStatus::Ok);
    BOOST_TEST(static_cast<bool>(joiner.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready));
    BOOST_TEST(new_token != old_token);
    captured_joiner.Open();
    BOOST_TEST(joiner.get() == ProcesserDriverStatus::Ok);
    PDSetProbe(true, {});
    BOOST_TEST(sche.UnregisterProcesserDriver(old_token) == ProcesserDriverStatus::StaleToken);
    ProcesserDriverSnapshot s;
    BOOST_TEST(sche.QueryProcesserDriver(ids[0], s) == ProcesserDriverStatus::Ok);
    BOOST_TEST(s.m_state == ProcesserDriverState::Bound);
    BOOST_TEST(s.m_token == new_token);
    BOOST_TEST(sche.UnregisterProcesserDriver(new_token) == ProcesserDriverStatus::Ok);
#endif
    BOOST_TEST(sche.UnregisterProcesserDriver(tokB) == ProcesserDriverStatus::Ok);
}

/* ---------- 6. 失败态 + 在途 Wake + 控制线程单 owner 清理 ---------- */
void CaseFailure(ProcesserDriverFailure expected_source, bool is_tick, bool is_throw)
{
    auto& sche = *Scheduler::GetInstance();
    EnsureRuntime(2);
    auto ids = sche.GetWorkerProcesserIds();
    const ProcesserId target = ids[0], other = ids[1];

    auto st = std::make_shared<FakeState>();
    Gate fail_gate, wake_gate, terminal_gate;
    st->tick_gate = is_tick ? &fail_gate : nullptr;
    st->wait_gate = is_tick ? nullptr : &fail_gate;
    st->wake_gate = &wake_gate;
    st->terminal_gate = &terminal_gate;
    st->query_on_wake = true;
    st->self_pid = target;
    st->tick_result = is_tick && !is_throw ? ProcesserDriverResult::Failed : ProcesserDriverResult::Ok;
    st->wait_result = !is_tick && !is_throw ? ProcesserDriverResult::Failed : ProcesserDriverResult::Ok;
    st->tick_throw = is_tick && is_throw;
    st->wait_throw = !is_tick && is_throw;
    st->throw_msg = "b1-fake-failure";
    auto drv = std::make_shared<FakeDriver>(st);

    /* FakeResource 真实接入 driver 全部 callback（RAII 进出计数） */
    auto res = std::make_shared<FakeResource>();
    st->res = res;

    ProcesserDriverToken tok{};
    PD_REQUIRE(Register(sche, target, drv, tok) == ProcesserDriverStatus::Ok);
    st->arm_gates.store(true, std::memory_order_release);

    /* 等失败 callback 进入 gate（在途）后才允许故障生效：
     * 先 arm gate → callback entered → failure_enabled → 放 gate。
     * Register 返回前的 Tick/Wait 一律 Ok，确定能进门。 */
    PD_REQUIRE(fail_gate.WaitEntered());
    st->failure_enabled.store(true, std::memory_order_release);
    /* 期间制造一次在途普通 Wake（helper 线程投递；Wake 同步阻塞在
     * 调用线程的 gate 上）。 */
    JoiningThread waker([&]{ PostOn(sche, target, []{}); });
    PD_REQUIRE(wake_gate.WaitEntered());
    BOOST_TEST(!res->TryBeginCleanup(std::this_thread::get_id()));
    int h, pend, comp, act, seq, viol;
    res->Snapshot(h, pend, comp, act, seq, viol);
    BOOST_TEST(act >= 2); BOOST_TEST(h == 2); BOOST_TEST(comp == 0);

    /* 释放失败 callback → 状态转 Failed */
    fail_gate.Open();
    ProcesserDriverSnapshot snap;
    PD_REQUIRE(WaitUntil([&]{
        return sche.QueryProcesserDriver(target, snap) == ProcesserDriverStatus::Ok &&
               snap.m_state == ProcesserDriverState::Failed;
    }));
    BOOST_TEST(snap.m_failure == expected_source);
    if (is_throw) {
        BOOST_TEST(static_cast<bool>(snap.m_exception));
        try { std::rethrow_exception(snap.m_exception); }
        catch (const std::exception& e) { BOOST_TEST(std::strcmp(e.what(), "b1-fake-failure") == 0); }
    } else {
        BOOST_TEST(!static_cast<bool>(snap.m_exception));
    }

    /* Failed 后不再 Tick/Wait；槽仍占用（在途 Wake 的 active 不计入
     * Tick/Wait 统计）。 */
    const int ticks = st->before.load() + st->after.load();
    const int waits = st->waits.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    BOOST_TEST(st->before.load() + st->after.load() == ticks);
    BOOST_TEST(st->waits.load() == waits);
    ProcesserDriverToken dummy{};
    BOOST_TEST(Register(sche, target, std::make_shared<FakeDriver>(std::make_shared<FakeState>()), dummy)
               == ProcesserDriverStatus::Occupied);

    /* 给失败 P 投一个 OS-gate 阻塞业务任务，确认确在目标 worker 开始 */
    WorkerFence other_fence(other);
    Gate biz_gate;
    std::atomic<ProcesserId> biz_pid{0};
    bbt::core::thread::CountDownLatch biz_started{1}, biz_done{1};
    PostOn(sche, target, [&, res]{
        auto p = Processer::TryGetLocalProcesser();
        biz_pid = p ? p->GetId() : 0;
        biz_started.Down();
        biz_gate.Wait();
        biz_done.Down();
    });
    PD_REQUIRE(LatchWait(biz_started));
    PD_REQUIRE(biz_pid.load() == target);
    other_fence.Open();

    /* async 同代解绑：终止 Wake 必须等待在途普通 Wake；解绑完成时目标
     * P 仍被业务 gate 占住——证明不需要 worker ack。 */
    auto fut = Async(std::launch::async, [&]{ return sche.UnregisterProcesserDriver(tok); });
    PD_REQUIRE(terminal_gate.WaitEntered());
    terminal_gate.Open();
    PD_REQUIRE(WaitUntil([&]{ return st->terminal_exited.load() == 1; }));
    BOOST_TEST(!res->TryBeginCleanup(std::this_thread::get_id()));
    BOOST_TEST(static_cast<bool>(
        fut.wait_for(std::chrono::milliseconds(300)) != std::future_status::ready));
    wake_gate.Open();
    waker.join();
    PD_REQUIRE(static_cast<bool>(
        fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready));
    BOOST_TEST(fut.get() == ProcesserDriverStatus::Ok);
    /* 此刻业务任务仍持有目标 P（biz_gate 未放）——解绑不依赖 worker */
    BOOST_TEST(static_cast<bool>(biz_done.WaitTimeout(10) != 0));

    /* 同步解绑后：控制线程单 owner 清理旧 fake 资源 */
    /* 同步解绑前（Detaching/在途仍在或已 Failed）：cleanup 拒绝且
     * 资源不变；解绑 Ok 后 MarkDetached，控制线程单 owner 推进。 */
    const int seq_before = res->AccessSeq();
    BOOST_TEST(!res->TryBeginCleanup(std::this_thread::get_id()));  // 缺少收据仍拒绝
    res->Snapshot(h, pend, comp, act, seq, viol);
    BOOST_TEST(h == 2);                     // 资源内容未变
    BOOST_TEST(comp == 0);

    res->MarkDetached();                    // Unregister 已 Ok
    BOOST_TEST(st->active_callbacks.load() == 0);
    PD_REQUIRE(res->TryBeginCleanup(std::this_thread::get_id()));

    /* 有界步进真消耗：close 一个仍非空 → close 完 pending 非空 →
     * drain 一个仍有 → 最终全收口且 2 个不同 completion 确实执行 */
    PD_REQUIRE(res->CloseOne(std::this_thread::get_id()));
    res->Snapshot(h, pend, comp, act, seq, viol);
    BOOST_TEST(h == 1); BOOST_TEST(pend == 1);
    PD_REQUIRE(res->CloseOne(std::this_thread::get_id()));
    PD_REQUIRE(res->DrainOne(std::this_thread::get_id()));
    res->Snapshot(h, pend, comp, act, seq, viol);
    BOOST_TEST(h == 0); BOOST_TEST(pend == 1); BOOST_TEST(comp == 1);
    PD_REQUIRE(res->DrainOne(std::this_thread::get_id()));
    res->Snapshot(h, pend, comp, act, seq, viol);
    BOOST_TEST(h == 0); BOOST_TEST(pend == 0); BOOST_TEST(comp == 2);
    {
        std::lock_guard<std::mutex> l(res->m);
        BOOST_TEST(res->completed[0] != res->completed[1]);
        BOOST_TEST(res->closed_handles.size() == 2u);
        BOOST_TEST(res->owner == std::this_thread::get_id());
        BOOST_TEST(res->pump_tid != res->owner);
        BOOST_TEST(res->visits[FakeResource::TickK] > 0);
        BOOST_TEST(res->visits[FakeResource::WakeK] > 0);
        if (!is_tick) BOOST_TEST(res->visits[FakeResource::WaitK] > 0);
    }
    BOOST_TEST(!res->pump_conflict);
    drv.reset();
    /* 解绑后旧资源零访问、零违例（严格等于，不是放宽计数） */
    /* 另一 worker 真实进展 */
    bbt::core::thread::CountDownLatch other_done{1};
    PostOn(sche, other, [&]{ other_done.Down(); });
    BOOST_TEST(LatchWait(other_done));
    biz_gate.Open();
    BOOST_TEST(LatchWait(biz_done));
    bbt::core::thread::CountDownLatch next_round{1};
    PostOn(sche, target, [&]{ next_round.Down(); });
    BOOST_TEST(LatchWait(next_round));
    res->Snapshot(h, pend, comp, act, seq, viol);
    BOOST_TEST(seq == seq_before);
    BOOST_TEST(viol == 0);
    BOOST_TEST(act == 0);
    BOOST_TEST(res->completion_effect.load() == 3);
}

/* ---------- 7. Wake 锁外重入与整链锁外 ---------- */
void CaseReentrancy()
{
    auto& sche = *Scheduler::GetInstance();
    EnsureRuntime(2);
    auto ids = sche.GetWorkerProcesserIds();
    PD_REQUIRE(ids.size() == 2u);

    /* 共享场景对象：先建好再绑定，全部活到同步解绑之后 */
    auto waiter = std::make_shared<CoWaiterProbe>();
    auto chan = std::make_shared<ChanProbe>();
    CleanupAction close_reader([chan]{ chan->Close(); });
    auto stage = std::make_shared<std::atomic<int>>(0);
    auto reenter_ok = std::make_shared<std::atomic<int>>(0);
    auto once = std::make_shared<std::atomic_bool>(true);
    auto notify_reenter = std::make_shared<std::atomic<int>>(99);
    auto chan_size = std::make_shared<std::atomic<size_t>>(99999);
    ProcesserId pid0 = ids[0];

    auto stA = std::make_shared<FakeState>();
    auto stB = std::make_shared<FakeState>();
    static thread_local bool notifying_this_activation = false;
    std::atomic<int> activation_a{0}, activation_b{0};
    stB->on_wake = [&] { if (notifying_this_activation) ++activation_b; };
    std::atomic<ProcesserId> registered_pid{0}, chan_pid{0};
    std::atomic_bool committed{false}, chan_committed{false};
    auto after_commit = [&] {
        auto pid = Processer::TryGetLocalProcesser()->GetId();
        if (registered_pid.load() == pid) committed = true;
        if (chan_pid.load() == pid &&
            chan->reader->SnapshotEventStatus() == static_cast<int>(POLLEVENT_LISTEN))
            chan_committed = true;
    };
    stA->on_after = after_commit;
    stB->on_after = after_commit;
    /* on_wake 绑定前配置一次，内部按 stage 分发；不再写 plain 成员 */
    stA->on_wake = [&, waiter, chan, stage, reenter_ok, once, notify_reenter, chan_size]{
        if (notifying_this_activation) ++activation_a;
        switch (stage->load(std::memory_order_acquire)) {
        case 1: {   /* adapter 重入：GetWorkers/Query/受控递归 Regist 一次 */
            if (!once->exchange(false)) break;
            auto wids = sche.GetWorkerProcesserIds();
            ProcesserDriverSnapshot s;
            bool query_ok = (sche.QueryProcesserDriver(pid0, s) == ProcesserDriverStatus::Ok);
            bool regist_ok = true;
            try { Post(sche, []{}); } catch (...) { regist_ok = false; }
            BBTATTR_COMM_UNUSED auto cid = GetLocalCoroutineId();
            if (!wids.empty() && query_ok && regist_ok)
                (*reenter_ok)++;
            break;
        }
        case 2: /* scope 退出后锁已释放：带载荷 Notify 返回 -1 不死锁 */
            *notify_reenter = waiter->Notify(bbt::coroutine::CoEventValue::From(1));
            break;
        case 3: /* Wake 内重入 chan.size()：外层 OS 锁已释放才能返回 */
            *chan_size = chan->size();
            break;
        default: break;
        }
    };
    auto drvA = std::make_shared<FakeDriver>(stA);
    auto drvB = std::make_shared<FakeDriver>(stB);
    ProcesserDriverToken tokA{}, tokB{};
    PD_REQUIRE(Register(sche, ids[0], drvA, tokA) == ProcesserDriverStatus::Ok);
    PD_REQUIRE(Register(sche, ids[1], drvB, tokB) == ProcesserDriverStatus::Ok);

    /* (a) 普通 Regist 实际触发 Wake，重入 adapter API */
    stage->store(1, std::memory_order_release);
    Post(sche, []{});
    BOOST_TEST(WaitUntil([&]{ return reenter_ok->load() >= 1; }));

    /* (b) 真实全局广播：确认 PARKED 后 Notify，两台 bound driver 的
     * Wake 都应观测到；Wake 内重入同一 waiter.Notify(value)。 */
    bbt::core::thread::CountDownLatch resumed{1};
    Post(sche, [&, waiter]{
        waiter->WaitWithTimeoutAndCallback(3000, [&] {
            registered_pid = Processer::TryGetLocalProcesser()->GetId();
            return true;
        });
        resumed.Down();
    });
    // on_registered 在 CommitYield 前；同 P 的 AfterSchedule 才是提交后确认。
    PD_REQUIRE(WaitUntil([&]{ return committed.load(); }));
    stage->store(2, std::memory_order_release);
    const int wake_a = stA->wakes.load(), wake_b = stB->wakes.load();
    notifying_this_activation = true;
    BOOST_TEST(waiter->Notify() == 0);
    notifying_this_activation = false;
    BOOST_TEST(activation_a.load() == 1);
    BOOST_TEST(activation_b.load() == 1);
    BOOST_TEST(LatchWait(resumed));
    BOOST_TEST(stA->wakes.load() > wake_a);
    BOOST_TEST(stB->wakes.load() > wake_b);
    BOOST_TEST(WaitUntil([&]{ return notify_reenter->load() != 99; }));
    BOOST_TEST(notify_reenter->load() == -1);

    /* (c) Chan 链：真实读者挂起后 TryWrite 的 _OnEnableRead→Notify→
     * 广播触发 Wake；Wake 内 chan.size() 证明外层 OS 锁已释放。 */
    bbt::core::thread::CountDownLatch read_done{1};
    Post(sche, [&, chan]{
        chan_pid = Processer::TryGetLocalProcesser()->GetId();
        int v = 0; chan->Read(v); read_done.Down();
    });
    PD_REQUIRE(WaitUntil([&]{ return chan_committed.load(); }));
    stage->store(3, std::memory_order_release);
    BOOST_TEST(chan->TryWrite(1) == 0);
    BOOST_TEST(WaitUntil([&]{ return chan_size->load() <= 1u; })); // 0或1取决于reader进度
    BOOST_TEST(LatchWait(read_done));
    stage->store(0, std::memory_order_release);

    /* 解绑后才销毁场景对象（栈变量此刻仍活着，双重保险） */
    BOOST_TEST(sche.UnregisterProcesserDriver(tokA) == ProcesserDriverStatus::Ok);
    BOOST_TEST(sche.UnregisterProcesserDriver(tokB) == ProcesserDriverStatus::Ok);
}

/* ---------- 8. steal + EVENT_WAIT 挂起迁移 ---------- */
void CaseStealMigrate()
{
    auto& sche = *Scheduler::GetInstance();
    EnsureRuntime(2);
    auto ids = sche.GetWorkerProcesserIds();
    const auto target = ids[0], other = ids[1];
    Gate target_pump, hold, other_hold, target_hold;
    auto st = std::make_shared<FakeState>();
    st->pump_gate = &target_pump;
    std::atomic<ProcesserId> registered_pid{0};
    std::atomic_bool committed{false};
    st->on_after = [&] {
        if (registered_pid.load() == target) committed = true;
    };
    ProcesserDriverToken tok{};
    PD_REQUIRE(Register(sche, target, std::make_shared<FakeDriver>(st), tok) == ProcesserDriverStatus::Ok);

    // 先在 other 的真实 Tick 关门，再投 target blocker；首投无 stealing 竞争。
    WorkerFence other_fence(other);
    bbt::core::thread::CountDownLatch occupying{1}, occ_done{1};
    std::atomic<ProcesserId> blocker_pid{0};
    PostOn(sche, target, [&] {
        blocker_pid = Processer::TryGetLocalProcesser()->GetId();
        occupying.Down(); hold.Wait(); occ_done.Down();
    });
    PD_REQUIRE(LatchWait(occupying));
    PD_REQUIRE(blocker_pid.load() == target);
    other_fence.Open();
    std::atomic<ProcesserId> ran_pid{0};
    bbt::core::thread::CountDownLatch ran{1};
    PostOn(sche, target, [&] {
        ran_pid = Processer::TryGetLocalProcesser()->GetId(); ran.Down();
    });
    PD_REQUIRE(LatchWait(ran));
    BOOST_TEST(ran_pid.load() == other);
    hold.Open();
    PD_REQUIRE(LatchWait(occ_done));
    PD_REQUIRE(WaitUntil([&]{ return st->before.load() > 0; }));
    BOOST_TEST(st->tick_pid.load() == target);

    // 反向布局：目标 P 停在真实 Tick，确保 other blocker 不会被 target 偷走。
    st->hold_pump = true;
    PostOn(sche, target, []{});
    PD_REQUIRE(target_pump.WaitEntered());
    BOOST_TEST(st->tick_pid.load() == target);
    bbt::core::thread::CountDownLatch other_in{1}, other_out{1};
    std::atomic<ProcesserId> other_blocker{0};
    PostOn(sche, other, [&] {
        other_blocker = Processer::TryGetLocalProcesser()->GetId();
        other_in.Down(); other_hold.Wait(); other_out.Down();
    });
    PD_REQUIRE(LatchWait(other_in));
    PD_REQUIRE(other_blocker.load() == other);
    st->hold_pump = false; target_pump.Open();

    auto waiter = std::make_shared<CoWaiterProbe>();
    std::atomic<ProcesserId> first_pid{0}, resume_pid{0};
    std::atomic<CoroutineId> co_id{0}, resume_co{0};
    bbt::core::thread::CountDownLatch resume_done{1};
    PostOn(sche, target, [&, waiter] {
        first_pid = Processer::TryGetLocalProcesser()->GetId();
        co_id = GetLocalCoroutineId();
        waiter->WaitWithTimeoutAndCallback(3000, [&] {
            registered_pid = Processer::TryGetLocalProcesser()->GetId();
            return true;
        });
        resume_pid = Processer::TryGetLocalProcesser()->GetId();
        resume_co = GetLocalCoroutineId(); resume_done.Down();
    });
    PD_REQUIRE(WaitUntil([&]{ return committed.load(); }));
    BOOST_TEST(first_pid.load() == target);
    // other 仍被实际确认的 blocker 占住，故目标 blocker 的实际归属可固定。
    bbt::core::thread::CountDownLatch tgt_in{1}, tgt_out{1};
    std::atomic<ProcesserId> target_blocker{0};
    PostOn(sche, target, [&] {
        target_blocker = Processer::TryGetLocalProcesser()->GetId();
        tgt_in.Down(); target_hold.Wait(); tgt_out.Down();
    });
    PD_REQUIRE(LatchWait(tgt_in));
    PD_REQUIRE(target_blocker.load() == target);
    other_hold.Open();
    PD_REQUIRE(LatchWait(other_out));
    BOOST_TEST(waiter->Notify() == 0);
    PD_REQUIRE(LatchWait(resume_done));
    BOOST_TEST(resume_pid.load() == other);
    BOOST_TEST(resume_pid.load() != target);
    BOOST_TEST(resume_co.load() == co_id.load());
    BOOST_TEST(resume_co.load() != 0u);
    target_hold.Open();
    PD_REQUIRE(LatchWait(tgt_out));
    BOOST_TEST(sche.UnregisterProcesserDriver(tok) == ProcesserDriverStatus::Ok);
}

/* ---------- 9. 无 driver 定向任务仍执行 ---------- */
void CaseNoDriverDirected()
{
    auto& sche = *Scheduler::GetInstance();
    EnsureRuntime(1);
    auto ids = sche.GetWorkerProcesserIds();
    bbt::core::thread::CountDownLatch done{1};
    PostOn(sche, ids[0], [&]{ done.Down(); });
    BOOST_TEST(LatchWait(done));
}

/* ---------- 10. Query 的 exception_ptr 析构在锁外 ---------- */
void CaseQueryDestructRelease()
{
    auto& sche = *Scheduler::GetInstance();
    EnsureRuntime(2);
    auto ids = sche.GetWorkerProcesserIds();

    /* 先绑一个正常 Bound driver，让 Query 走真实 binding 锁路径 */
    auto st = std::make_shared<FakeState>();
    auto drv = std::make_shared<FakeDriver>(st);
    ProcesserDriverToken tok{};
    PD_REQUIRE(Register(sche, ids[0], drv, tok) == ProcesserDriverStatus::Ok);

    /* out_snapshot.m_exception 独占最后一份 payload：deleter 重入
     * 同一 target 的 Query。清掉所有其它引用后再覆盖。 */
    std::atomic<int> dtor_count{0}, reenter_ok{0};
    ProcesserId self = ids[0];
    auto payload = std::shared_ptr<int>(new int(0), [&](int* p){
        ProcesserDriverSnapshot s2;
        if (Scheduler::GetInstance()->QueryProcesserDriver(self, s2)
                == ProcesserDriverStatus::Ok &&
            s2.m_state == ProcesserDriverState::Bound)
            ++reenter_ok;
        ++dtor_count;
        delete p;
    });

    ProcesserDriverSnapshot out;
    try { throw std::runtime_error{"held"}; }
    catch (...) { out.m_exception = std::current_exception(); }
    /* 把 payload 挂进 exception 的捕获对象：用 shared_ptr 自定义
     * exception 承载。 */
    struct ExWithPayload : std::exception {
        std::shared_ptr<int> payload;
        const char* what() const noexcept override { return "held"; }
    };
    ExWithPayload ex; ex.payload = payload;
    out.m_exception = std::make_exception_ptr(ex);
    payload.reset();
    ex.payload.reset();       // 清掉其余引用：exception 独占最后一份
    BOOST_TEST(dtor_count.load() == 0);

    /* Query 覆盖 out：生产实现锁内拷局部、锁外 move 赋值，
     * out 旧 exception（含 payload 最后引用）在锁外析构并重入 Query。 */
    BOOST_TEST(sche.QueryProcesserDriver(self, out) == ProcesserDriverStatus::Ok);
    BOOST_TEST(dtor_count.load() == 1);     // 确切一次
    BOOST_TEST(reenter_ok.load() == 1);     // 重入成功、返回 Ok/Bound

    /* 再 Query 不重复析构 */
    BOOST_TEST(sche.QueryProcesserDriver(self, out) == ProcesserDriverStatus::Ok);
    BOOST_TEST(dtor_count.load() == 1);

    BOOST_TEST(sche.UnregisterProcesserDriver(tok) == ProcesserDriverStatus::Ok);
}

BOOST_AUTO_TEST_CASE(dispatch)
{
    const char* mode_env = std::getenv("BBT_PD_MODE");
    PD_REQUIRE(mode_env != nullptr);
    const std::string m = mode_env;
    if (m == "api_boundaries")        CaseApiBoundaries();
    else if (m == "tick_sequence")    CaseTickSequence();
    else if (m == "wait_wakeup")      CaseWaitWakeup();
    else if (m == "unregister_waits") CaseUnregisterWaits();
    else if (m == "self_detach")      CaseSelfDetach();
    else if (m == "fail_tick_ret")    CaseFailure(ProcesserDriverFailure::Tick, true, false);
    else if (m == "fail_tick_throw")  CaseFailure(ProcesserDriverFailure::Tick, true, true);
    else if (m == "fail_wait_ret")    CaseFailure(ProcesserDriverFailure::Wait, false, false);
    else if (m == "fail_wait_throw")  CaseFailure(ProcesserDriverFailure::Wait, false, true);
    else if (m == "reentrancy")       CaseReentrancy();
    else if (m == "steal_migrate")    CaseStealMigrate();
    else if (m == "no_driver_directed") CaseNoDriverDirected();
    else if (m == "query_destruct_release") CaseQueryDestructRelease();
    else BOOST_FAIL("unknown mode: " << m);
    std::cout << "PD_REQUIRE_VISITS=" << require_visits << std::endl;
}

BOOST_AUTO_TEST_SUITE_END()
