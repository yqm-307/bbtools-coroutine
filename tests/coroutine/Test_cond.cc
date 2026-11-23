#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <mutex>
#include <atomic>
#include <array>
#include <thread>
#include <chrono>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>

/**
 * 进程寿命模型：runtime 只初始化一次，重复 Start 抛 std::logic_error。
 * 每个测试文件就是一个可执行，这里把用例内的 Start() 收敛为进程内一次初始化。
 */
namespace {
void EnsureRuntime()
{
    static std::once_flag once;
    std::call_once(once, []() { bbt::coroutine::detail::Scheduler::GetInstance()->Start(); });
}
/**
 * 截止时间保护的让权自旋：仅在协程内使用，等待某个已发生的事实变得可见。
 * 超时返回 false 交由调用方断言失败；不引入正确性依赖的固定等待。
 */
bool SpinUntilCo(const std::function<bool()>& ready, int timeout_ms)
{
    auto deadline = bbt::core::clock::nowAfter(bbt::core::clock::milliseconds(timeout_ms));
    while (!ready()) {
        if (bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(deadline))
            return false;
        bbtco_yield;
    }
    return true;
}

/**
 * 主线程有界等待收口原子计数：等待超时判失败，不无限阻塞测试进程。
 */
void WaitCountBounded(std::atomic_int& count, int target, int timeout_ms)
{
    auto deadline = bbt::core::clock::nowAfter(bbt::core::clock::milliseconds(timeout_ms));
    while (count.load() < target) {
        if (bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(deadline)) {
            BOOST_ERROR("wait count timed out");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
}

using namespace bbt::coroutine;

#define PrintTime(flag)                                                                                                \
    printf(                                                                                                            \
        "标记点=[%s]   协程id=[%ld] 时间戳=[%ld]\n",                                                                   \
        flag,                                                                                                          \
        GetLocalCoroutineId(),                                                                                         \
        bbt::core::clock::now<>().time_since_epoch().count());

BOOST_AUTO_TEST_SUITE(CoCondTest)

BOOST_AUTO_TEST_CASE(t_begin)
{
    EnsureRuntime();
}

BOOST_AUTO_TEST_CASE(t_cond_multi)
{
    std::atomic_bool run_in_co{false};

    bbtco[&run_in_co]()
    {
        auto co1 = sync::CoWaiter::Create();
        auto co2 = sync::CoWaiter::Create();
        Assert(co1 != nullptr && co2 != nullptr);

        bbtco[&run_in_co, co1, co2]()
        {
            BOOST_ASSERT(!run_in_co.load());
            run_in_co = true;
            co1->Notify();
            co2->Wait();
            BOOST_ASSERT(!run_in_co.load());
            run_in_co = true;
            co1->Notify();
            co1->Notify();
            co2->Wait();
        };

        co1->Wait();
        BOOST_ASSERT(run_in_co.load());
        run_in_co = false;
        co2->Notify();
        co1->Wait();
        BOOST_ASSERT(run_in_co.load());
        run_in_co = false;
        co2->Notify();
    };

    auto max_end_ts = bbt::core::clock::nowAfter(bbt::core::clock::milliseconds(100));
    while (!bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(max_end_ts)) {
        std::this_thread::sleep_for(bbt::core::clock::milliseconds(10));
    }
}

BOOST_AUTO_TEST_CASE(t_cond_wait_with_timeout)
{
    const int wait_ms = 200;
    std::atomic_int a{0};
    std::atomic_uint64_t elapsed_ms{0};
    bbtco[&]()
    {
        auto cond = sync::CoWaiter::Create();
        auto begin = bbt::core::clock::gettime();
        cond->WaitWithTimeout(wait_ms);
        auto end = bbt::core::clock::gettime();
        elapsed_ms.store(end - begin, std::memory_order_release);
        a.fetch_add(1, std::memory_order_release);
    };

    auto max_end_ts = bbt::core::clock::nowAfter(bbt::core::clock::milliseconds(500));
    while (a.load(std::memory_order_acquire) <= 0
           && !bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(max_end_ts))
        std::this_thread::sleep_for(bbt::core::clock::milliseconds(10));

    BOOST_CHECK_EQUAL(a.load(std::memory_order_acquire), 1);
    BOOST_CHECK_GE(elapsed_ms.load(std::memory_order_acquire), static_cast<uint64_t>(wait_ms));
}

BOOST_AUTO_TEST_CASE(t_cond_wait)
{
    std::mutex lock;
    const int nmax_co = 1000;
    std::atomic_int ncount{0};
    bbt::core::thread::CountDownLatch l2{nmax_co};

    bbtco[&]()
    {
        auto cond = bbtco_make_cocond();

        for (int i = 0; i < nmax_co; ++i) {
            bbtco[&]()
            {
                cond->Wait();
                ncount++;
                l2.Down();
            };
        }

        sleep(1);
        cond->NotifyAll();
    };

    l2.Wait();
    BOOST_CHECK(ncount == nmax_co);
}

/**
 * =================== P1: WaitFor(ms) ===================
 */

/**
 * WaitFor(ms) 超时后正常返回
 */
BOOST_AUTO_TEST_CASE(t_waitfor_timeout)
{
    bbt::core::thread::CountDownLatch l{1};
    int result = -999;

    bbtco[&]()
    {
        auto cond = bbtco_make_cocond();
        auto begin = bbt::core::clock::gettime_mono();
        result = cond->WaitFor(100);
        auto elapsed = bbt::core::clock::gettime_mono() - begin;

        /**
         * 超时不应返回 0
         */
        BOOST_TEST(result != 0);

        /**
         * 至少等了 ~100ms
         */
        BOOST_TEST(elapsed >= 90);

        /**
         * 不应等太久
         */
        BOOST_TEST(elapsed < 1000);
        l.Down();
    };

    l.Wait();
    BOOST_TEST(result != 0);
}

/**
 * WaitFor(ms) 在 NotifyOne 之前被唤醒
 */
BOOST_AUTO_TEST_CASE(t_waitfor_notifyone)
{
    bbt::core::thread::CountDownLatch l{1};
    int result = -999;

    bbtco[&]()
    {
        auto cond = bbtco_make_cocond();

        bbtco[cond, &result, &l]()
        {
            auto begin = bbt::core::clock::gettime_mono();
            result = cond->WaitFor(500);
            auto elapsed = bbt::core::clock::gettime_mono() - begin;

            /**
             * 应该在 500ms 前被唤醒
             */
            BOOST_TEST(elapsed < 400);
            l.Down();
        };

        bbtco_sleep(50);
        cond->NotifyOne();
    };

    l.Wait();

    /**
     * NotifyOne 唤醒，返回 0
     */
    BOOST_TEST(result == 0);
}

/**
 * WaitFor(ms) 在 NotifyAll 之前被唤醒
 */
BOOST_AUTO_TEST_CASE(t_waitfor_notifyall)
{
    const int n = 50;
    bbt::core::thread::CountDownLatch l{n};
    std::atomic_int woken{0};

    bbtco[&]()
    {
        auto cond = bbtco_make_cocond();

        for (int i = 0; i < n; ++i) {
            bbtco[cond, &woken, &l]()
            {
                int ret = cond->WaitFor(500);
                if (ret == 0)
                    woken++;
                l.Down();
            };
        }

        bbtco_sleep(100);
        cond->NotifyAll();
    };

    l.Wait();
    BOOST_TEST(woken == n);
}

/**
 * =================== P1: NotifyOne 空队列 ===================
 */

/**
 * NotifyOne 空队列返回 -1
 */
BOOST_AUTO_TEST_CASE(t_notifyone_empty)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco[&]()
    {
        auto cond = bbtco_make_cocond();
        int ret = cond->NotifyOne();

        /**
         * 空队列返回 -1
         */
        BOOST_TEST(ret == -1);
        l.Down();
    };

    l.Wait();
}

/**
 * NotifyOne 后 WaitFor 仍可正常使用
 */
BOOST_AUTO_TEST_CASE(t_notifyone_empty_then_use)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco[&]()
    {
        auto cond = bbtco_make_cocond();

        /**
         * 空队列 NotifyOne 应该返回 -1
         */
        int ret = cond->NotifyOne();
        BOOST_TEST(ret == -1);

        /**
         * 之后正常 WaitFor + NotifyOne 仍然工作
         */
        bbtco[cond]()
        {
            bbtco_sleep(50);
            cond->NotifyOne();
        };

        ret = cond->WaitFor(200);
        BOOST_TEST(ret == 0);
        l.Down();
    };

    l.Wait();
}

/**
 * Wait + NotifyOne 单协程
 */
BOOST_AUTO_TEST_CASE(t_wait_notifyone_single)
{
    bbt::core::thread::CountDownLatch l{1};
    int woken = 0;

    bbtco[&]()
    {
        auto cond = bbtco_make_cocond();

        bbtco[cond, &woken, &l]()
        {
            cond->Wait();
            woken = 1;
            l.Down();
        };

        bbtco_sleep(30);
        int ret = cond->NotifyOne();
        BOOST_TEST(ret == 0);
    };

    l.Wait();
    BOOST_TEST(woken == 1);
}

/**
 * 异常安全：已 Cancel 的 Waiter 不会被 Notify 唤醒
 */
BOOST_AUTO_TEST_CASE(t_cancelled_waiter_skipped)
{
    using namespace bbt::coroutine::sync;

    auto waiter = CoWaiter::Create();

    /**
     * 模拟异常路径取消
     */
    waiter->Cancel();

    /**
     * Notify 应返回 -1（跳过已取消的 waiter）
     */
    int ret = waiter->Notify();
    BOOST_TEST(ret == -1);
}

/**
 * =================== 锁外发布保护：首胜保留与对立取消/超时 ===================
 * wait/wake 热路径改造（首胜声明保留 + 已挂起完成的锁外延迟唤醒）的确定性回归。
 * 阶段证明来自挂起登记回调与截止时间自旋，不以 sleep 作为正确性依据。
 */

/**
 * 单次 Notify 只声明一次完成，且不跨槽泄漏：4 个 waiter 全部完成挂起登记后
 * 只通知槽 0，其余 3 槽必须按超时收口；每个槽恢复恰一次。
 * 抓错：一次 Notify 唤醒多个等待者（重复交付）、恢复两次（完成被重复发布）、
 * 未通知槽被幻影唤醒、已声明完成被本槽定时器翻转。
 * 断言全部在主线程做出。
 */
BOOST_AUTO_TEST_CASE(t_notify_claims_exactly_once_no_cross_wake)
{
    struct SlotState {
        sync::CoWaiter::SPtr waiter{sync::CoWaiter::Create()};
        std::atomic_int registered{0};
        std::atomic_int ret{-999};
        std::atomic_int resumes{0};
    };
    struct CaseState {
        std::array<SlotState, 4> slots;
        std::atomic_int done{0};
        std::atomic_int stage_ok{0};
        std::atomic_int notify_ret{-999};
    };
    auto state = std::make_shared<CaseState>();

    for (int i = 0; i < 4; ++i) {
        bbtco[state, i]()
        {
            auto& slot = state->slots[i];
            slot.ret.store(slot.waiter->WaitWithTimeoutAndCallback(400, [&slot]() {
                slot.registered.store(1, std::memory_order_release);
                return true;
            }));
            slot.resumes.fetch_add(1);
            state->done.fetch_add(1);
        };
    }

    bbtco[state]()
    {
        /**
         * 阶段门槛：4 槽全部完成挂起登记后才通知
         */
        bool ready = SpinUntilCo(
            [&state]() {
                for (int i = 0; i < 4; ++i) {
                    if (state->slots[i].registered.load(std::memory_order_acquire) != 1)
                        return false;
                }
                return true;
            },
            3000);
        state->stage_ok.store(ready ? 1 : 0);
        state->notify_ret.store(state->slots[0].waiter->Notify());
    };

    WaitCountBounded(state->done, 4, 8000);

    BOOST_CHECK_EQUAL(state->stage_ok.load(), 1);
    BOOST_CHECK_EQUAL(state->notify_ret.load(), 0);
    BOOST_CHECK_EQUAL(state->slots[0].ret.load(), 0);
    BOOST_CHECK_EQUAL(state->slots[0].resumes.load(), 1);
    for (int i = 1; i < 4; ++i) {
        BOOST_CHECK_EQUAL(state->slots[i].ret.load(), 1);
        BOOST_CHECK_EQUAL(state->slots[i].resumes.load(), 1);
    }
}

/**
 * 空队列 NotifyOne 不保留通知：通知先于等待发生时不得为后来的等待者
 * 兑现幻影唤醒。
 * 抓错：通知被错误记忆并交付给后续等待（首胜声明只属于已登记的等待者）。
 * 断言全部在主线程做出。
 */
BOOST_AUTO_TEST_CASE(t_notifyone_before_wait_not_sticky)
{
    struct CaseState {
        sync::CoCond::SPtr cond{sync::CoCond::Create()};
        std::atomic_int notify_empty_ret{-999};
        std::atomic_int waiter_ret{-999};
        std::atomic_int settled{0};
        std::atomic_int stage_ok{0};
    };
    auto state = std::make_shared<CaseState>();

    bbtco[state]()
    {
        state->notify_empty_ret.store(state->cond->NotifyOne());

        bbtco[state]()
        {
            state->waiter_ret.store(state->cond->WaitFor(150));
            state->settled.store(1);
        };

        state->stage_ok.store(SpinUntilCo([&state] { return state->settled.load() == 1; }, 3000) ? 1 : 0);
    };

    WaitCountBounded(state->settled, 1, 8000);
    BOOST_CHECK_EQUAL(state->notify_empty_ret.load(), -1);
    BOOST_CHECK_EQUAL(state->stage_ok.load(), 1);
    BOOST_CHECK_EQUAL(state->waiter_ret.load(), 1);
}

/**
 * 已声明的完成不被对立取消推翻：登记完成后 Notify 首胜（0），随后到达的
 * Cancel 只能让后续 Notify 跳过，不得吞掉已交付的恢复或造成二次恢复。
 * 抓错：Cancel 越过已声明完成（丢失唤醒）、完成被取消重复发布（恢复两次）。
 * 复用与迟到通知不污染下一轮已由 cowaiter 既有用例承接，此处不再重复。
 * 断言全部在主线程做出。
 */
BOOST_AUTO_TEST_CASE(t_cancel_after_notify_claim_pairing)
{
    struct CaseState {
        sync::CoWaiter::SPtr waiter{sync::CoWaiter::Create()};
        std::atomic_int registered{0};
        std::atomic_int notify_ret{-999};
        std::atomic_int wait_ret{-999};
        std::atomic_int resumes{0};
        std::atomic_int settled{0};
        std::atomic_int stage_ok{0};
    };
    auto state = std::make_shared<CaseState>();

    bbtco[state]()
    {
        state->wait_ret.store(state->waiter->WaitWithTimeoutAndCallback(400, [&state]() {
            state->registered.store(1);
            return true;
        }));
        state->resumes.fetch_add(1);
        state->settled.store(1);
    };

    bbtco[state]()
    {
        state->stage_ok.store(SpinUntilCo([&state] { return state->registered.load() == 1; }, 3000) ? 1 : 0);
        state->notify_ret.store(state->waiter->Notify());
        state->waiter->Cancel();
    };

    WaitCountBounded(state->settled, 1, 8000);
    BOOST_CHECK_EQUAL(state->stage_ok.load(), 1);
    BOOST_CHECK_EQUAL(state->notify_ret.load(), 0);
    BOOST_CHECK_EQUAL(state->wait_ret.load(), 0);
    BOOST_CHECK_EQUAL(state->resumes.load(), 1);
    BOOST_CHECK_EQUAL(state->waiter->Notify(), -1);
}

BOOST_AUTO_TEST_CASE(t_end) {}

BOOST_AUTO_TEST_SUITE_END()
