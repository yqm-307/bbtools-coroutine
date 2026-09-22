/**
 * @file Test_completion_signal.cc
 * @brief #347 C1 CompletionSignal 验收（C-01..C-05 中适用项）：
 *        Complete-before-Wait、CO_SUSPEND 屏障后 Complete、结果发布可见性、
 *        登记/挂起竞争、重复完成与多线程完成竞争、并发 Wait 拒绝、
 *        预取消/等待中取消/协作取消、deadline 过期、超时后再 Wait、
 *        完成与取消竞争的确定优先级、非协程 InvalidContext、
 *        Stop 后晚到 Complete、旧代际 RuntimeUnavailable。
 *
 * 契约：build-design-contract/agent-docs/2026-09-17-service-runtime-contract-v1.md §C1
 * 决议优先级固定为 已完成 > 取消 > 超时，测试按确定性断言，不靠时序运气。
 *
 * #347③：CompletionSignal 组合复用 CoWaiter 窄接口——挂起胜负在仲裁点
 * （唤醒原因掩码）一次定死。追加用例：登记前完成不丢唤醒、超时先胜不
 * 改判、取消/超时/完成掩码三向可区分、旧订阅晚到不污染、Stop 不展开
 * 栈下跨挂起状态不持有已释放事件。
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/object/CoObject.hpp>
#include <bbt/coroutine/sync/CompletionSignal.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

namespace
{

struct ConfigSnapshot
{
    size_t threads{0};
    bool stack_protect{false};
    bool saved{false};
} g_cfg;

std::atomic_bool g_started{false};
std::shared_ptr<CompletionSignal> g_old_gen_signal;

/* 确定性屏障：自旋等待真实条件成立（非定时 sleep），有界防挂死。 */
bool WaitUntil(const std::function<bool()>& pred, int budget_ms = 10000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

/* 协程已进入事件挂起（CO_SUSPEND 先于 YieldWithCallback 内的登记回调置位，
 * 观察到即说明等待事件已建好，Complete/取消必走事件状态机路径）。 */
bool CoroutineSuspended(std::atomic<Coroutine*>& co)
{
    auto* p = co.load();
    return p != nullptr && p->GetStatus() == CoroutineStatus::CO_SUSPEND;
}
}

BOOST_AUTO_TEST_SUITE(CompletionSignalTest)

/* Scheduler 未运行时构造抛 std::logic_error（用例有序，本例在 Start 前） */
BOOST_AUTO_TEST_CASE(t_ctor_requires_running_scheduler)
{
    BOOST_CHECK_EQUAL(CurrentRuntimeGeneration(), 0u);
    BOOST_CHECK_THROW(CompletionSignal sig, std::logic_error);
}

BOOST_AUTO_TEST_CASE(t_begin)
{
    auto* cfg = g_bbt_coroutine_config.get();
    if (!g_cfg.saved) {
        g_cfg.threads = cfg->m_cfg_static_thread_num;
        cfg->m_cfg_static_thread_num = 1;
        cfg->m_cfg_stack_protect = false;
        g_cfg.saved = true;
    }

    g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    g_started.store(true);
    BOOST_REQUIRE(g_scheduler->IsRunning());
}

/* C-01：Complete 先于 Wait 不丢通知；C-02：重复完成返回 false；
 * C-05：普通线程 Complete 合法 */
BOOST_AUTO_TEST_CASE(t_complete_before_wait)
{
    CompletionSignal sig;
    BOOST_CHECK(sig.Complete());
    BOOST_CHECK(!sig.Complete());

    bbt::core::thread::CountDownLatch done{1};
    std::atomic<WaitStatus> st{};
    bbtco [&]() {
        st.store(sig.Wait(WaitOptions{}));
        done.Down();
    };
    done.Wait();
    BOOST_CHECK(st.load() == WaitStatus::Completed);
}

/* C-01：等待中 Complete 正常唤醒 */
BOOST_AUTO_TEST_CASE(t_wait_then_complete)
{
    CompletionSignal sig;
    bbt::core::thread::CountDownLatch entered{1}, done{1};
    std::atomic<WaitStatus> st{};

    bbtco [&]() {
        entered.Down();
        st.store(sig.Wait(WaitOptions{}));
        done.Down();
    };

    entered.Wait();
    BOOST_CHECK(sig.Complete());
    done.Wait();
    BOOST_CHECK(st.load() == WaitStatus::Completed);
}

/* C-01：Complete 与 Wait 入口并发（覆盖登记/挂起竞争窗口），
 * 结果必须确定是 Completed，不丢通知 */
BOOST_AUTO_TEST_CASE(t_complete_races_wait_entry)
{
    for (int i = 0; i < 200; ++i) {
        CompletionSignal sig;
        bbt::core::thread::CountDownLatch entered{1}, done{1};
        std::atomic<WaitStatus> st{};

        bbtco [&]() {
            entered.Down();
            st.store(sig.Wait(WaitOptions{}));
            done.Down();
        };

        entered.Wait();
        sig.Complete();
        done.Wait();
        BOOST_CHECK(st.load() == WaitStatus::Completed);
    }
}

/* C-01：以等待者 CO_SUSPEND 为屏障的确定性 Complete。
 * CO_SUSPEND 在 YieldWithCallback 内置位、先于事件登记回调执行；
 * 观察到即说明 m_wait_event 已在锁内发布、事件对象已创建。此时
 * Complete 必走事件状态机：触发落在 CommitPark 之前被 PENDING 阶段
 * 吸收（协程被立即重新入队），落在 PARKED 之后则直接完成——两种
 * 时序的决议都确定是 Completed，不丢通知。本用例确定性覆盖
 * 「登记与挂起之间到达的 Complete」，与重复型用例互为证据。 */
BOOST_AUTO_TEST_CASE(t_complete_after_registration_barrier)
{
    CompletionSignal sig;
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<Coroutine*> co_p{nullptr};
    std::atomic<WaitStatus> st{};

    bbtco [&]() {
        co_p.store(g_bbt_tls_coroutine_co);
        st.store(sig.Wait(WaitOptions{}));
        done.Down();
    };

    BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
    BOOST_CHECK(sig.Complete());
    done.Wait();
    BOOST_CHECK(st.load() == WaitStatus::Completed);
}

/* C-01：结果发布可见——Complete 前写入的非原子载荷，等待者在 Wait
 * 返回 Completed 后必须读到已发布值。
 * 同步链：发布写 -> Complete() 锁内置位 m_completed -> Wait 决议在
 * 同一把锁内读 m_completed，release/acquire 由该互斥建立，载荷随
 * 完成状态可见（契约 §C1：先写业务结果再 Complete）。
 * 载荷为非原子复合值且与初值不同，任何未建立同步的读都会断言失败。 */
BOOST_AUTO_TEST_CASE(t_result_visible_to_waiter)
{
    struct Payload {
        std::uint64_t seq;
        std::string   text;
    };

    CompletionSignal sig;
    Payload payload{0, "unset"};    /* 非原子载荷：可见性完全依赖信号内部互斥 */
    Payload observed{0, "unset"};

    bbt::core::thread::CountDownLatch done{1};
    std::atomic<Coroutine*> co_p{nullptr};
    std::atomic<WaitStatus> st{};

    bbtco [&]() {
        co_p.store(g_bbt_tls_coroutine_co);
        const WaitStatus w = sig.Wait(WaitOptions{});
        if (w == WaitStatus::Completed)
            observed = payload;     /* Wait 返回后读已发布载荷 */
        st.store(w);
        done.Down();
    };

    BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));

    /* 先写业务结果再 Complete（契约 §C1：不能先 Complete 再填结果） */
    payload.seq  = 0xC001u;
    payload.text = "completion-result-published";
    BOOST_CHECK(sig.Complete());

    done.Wait();
    BOOST_CHECK(st.load() == WaitStatus::Completed);
    BOOST_CHECK_EQUAL(observed.seq, 0xC001u);
    BOOST_CHECK_EQUAL(observed.text, "completion-result-published");
}

/* C-02：多线程 Complete 竞争至多一次成功 */
BOOST_AUTO_TEST_CASE(t_concurrent_complete_single_winner)
{
    CompletionSignal sig;
    constexpr int kThreads = 8;
    bbt::core::thread::CountDownLatch go{1};
    std::atomic<int> winners{0};
    std::vector<std::thread> threads;

    for (int i = 0; i < kThreads; ++i)
        threads.emplace_back([&]() {
            go.Wait();
            if (sig.Complete())
                winners.fetch_add(1);
        });

    go.Down();
    for (auto& t : threads)
        t.join();
    BOOST_CHECK_EQUAL(winners.load(), 1);
}

/* C-02：并发第二个 Wait 返回 AlreadyWaiting，不排队 */
BOOST_AUTO_TEST_CASE(t_already_waiting)
{
    CompletionSignal sig;
    bbt::core::thread::CountDownLatch a_entered{1}, a_done{1}, b_done{1};
    std::atomic<Coroutine*> co_a{nullptr};
    std::atomic<WaitStatus> st_a{}, st_b{};

    bbtco [&]() {
        co_a.store(g_bbt_tls_coroutine_co);
        a_entered.Down();
        st_a.store(sig.Wait(WaitOptions{}));
        a_done.Down();
    };

    a_entered.Wait();
    /* 必须确认 A 已占据等待位再放 B，否则两者竞争结果不确定 */
    BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_a); }));

    bbtco [&]() {
        st_b.store(sig.Wait(WaitOptions{}));
        b_done.Down();
    };
    b_done.Wait();
    BOOST_CHECK(st_b.load() == WaitStatus::AlreadyWaiting);

    BOOST_CHECK(sig.Complete());
    a_done.Wait();
    BOOST_CHECK(st_a.load() == WaitStatus::Completed);
}

/* C-03：进入 Wait 前 token 已取消 → Cancelled（不挂起） */
BOOST_AUTO_TEST_CASE(t_pre_cancelled)
{
    CompletionSignal sig;
    CancellationSource source;
    source.RequestCancel();

    WaitOptions opt;
    opt.cancel = source.Token();

    bbt::core::thread::CountDownLatch done{1};
    std::atomic<WaitStatus> st{};
    bbtco [&]() {
        st.store(sig.Wait(opt));
        done.Down();
    };
    done.Wait();
    BOOST_CHECK(st.load() == WaitStatus::Cancelled);
}

/* C-03：等待中 token 取消 → Cancelled（确定性：确认挂起后才取消） */
BOOST_AUTO_TEST_CASE(t_cancel_during_wait)
{
    CompletionSignal sig;
    CancellationSource source;
    WaitOptions opt;
    opt.cancel = source.Token();

    bbt::core::thread::CountDownLatch entered{1}, done{1};
    std::atomic<Coroutine*> co_p{nullptr};
    std::atomic<WaitStatus> st{};

    bbtco [&]() {
        co_p.store(g_bbt_tls_coroutine_co);
        entered.Down();
        st.store(sig.Wait(opt));
        done.Down();
    };

    entered.Wait();
    BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
    source.RequestCancel();
    done.Wait();
    BOOST_CHECK(st.load() == WaitStatus::Cancelled);
}

/* C-03：当前协程协作取消同样终止等待。
 * 协程 RequestCancel 以 TIMEOUT 标志唤醒事件，结果为 Cancelled 证明
 * 决议顺序中取消判定先于超时映射。 */
BOOST_AUTO_TEST_CASE(t_coroutine_cancel_during_wait)
{
    CompletionSignal sig;
    bbt::core::thread::CountDownLatch entered{1}, done{1};
    std::atomic<Coroutine*> co_p{nullptr};
    std::atomic<WaitStatus> st{};

    bbtco [&]() {
        co_p.store(g_bbt_tls_coroutine_co);
        entered.Down();
        st.store(sig.Wait(WaitOptions{}));
        done.Down();
    };

    entered.Wait();
    BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
    co_p.load()->RequestCancel();
    done.Wait();
    BOOST_CHECK(st.load() == WaitStatus::Cancelled);
}

/* C-03：完成与取消竞争取确定优先级——完成优先。
 * 两段断言：(1) 进入前已完成且 token 已取消 → Completed；
 * (2) 挂起中先 Complete 再取消 token → Completed（唤醒原因不论，
 * 决议时完成可见即胜）。 */
BOOST_AUTO_TEST_CASE(t_completed_beats_cancel)
{
    {
        CompletionSignal sig;
        CancellationSource source;
        source.RequestCancel();
        BOOST_CHECK(sig.Complete());

        WaitOptions opt;
        opt.cancel = source.Token();
        bbt::core::thread::CountDownLatch done{1};
        std::atomic<WaitStatus> st{};
        bbtco [&]() {
            st.store(sig.Wait(opt));
            done.Down();
        };
        done.Wait();
        BOOST_CHECK(st.load() == WaitStatus::Completed);
    }

    {
        CompletionSignal sig;
        CancellationSource source;
        WaitOptions opt;
        opt.cancel = source.Token();

        bbt::core::thread::CountDownLatch entered{1}, done{1};
        std::atomic<Coroutine*> co_p{nullptr};
        std::atomic<WaitStatus> st{};
        bbtco [&]() {
            co_p.store(g_bbt_tls_coroutine_co);
            entered.Down();
            st.store(sig.Wait(opt));
            done.Down();
        };

        entered.Wait();
        BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
        /* 完成先发布：取消随后到达也改变不了本次决议 */
        BOOST_CHECK(sig.Complete());
        source.RequestCancel();
        done.Wait();
        BOOST_CHECK(st.load() == WaitStatus::Completed);
    }
}

/* C-03：deadline 已过期立即返回 TimedOut */
BOOST_AUTO_TEST_CASE(t_expired_deadline)
{
    CompletionSignal sig;
    WaitOptions opt;
    opt.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);

    bbt::core::thread::CountDownLatch done{1};
    std::atomic<WaitStatus> st{};
    bbtco [&]() {
        st.store(sig.Wait(opt));
        done.Down();
    };
    done.Wait();
    BOOST_CHECK(st.load() == WaitStatus::TimedOut);
}

/* C-03：超时只结束本次等待；完成状态保留，再次 Wait 可观察后续完成 */
BOOST_AUTO_TEST_CASE(t_timeout_then_rewait)
{
    CompletionSignal sig;
    WaitOptions short_opt;
    short_opt.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);

    bbt::core::thread::CountDownLatch first_done{1}, done{1};
    std::atomic<WaitStatus> st1{}, st2{};

    bbtco [&]() {
        st1.store(sig.Wait(short_opt));
        first_done.Down();
        st2.store(sig.Wait(WaitOptions{}));
        done.Down();
    };

    first_done.Wait();
    BOOST_CHECK(st1.load() == WaitStatus::TimedOut);

    BOOST_CHECK(sig.Complete());
    done.Wait();
    BOOST_CHECK(st2.load() == WaitStatus::Completed);
}

/* C-05：普通线程 Wait 一律 InvalidContext，即便信号已完成
 *（环境检查先于完成检查） */
BOOST_AUTO_TEST_CASE(t_wait_on_plain_thread_invalid_context)
{
    CompletionSignal sig;
    BOOST_CHECK(sig.Wait(WaitOptions{}) == WaitStatus::InvalidContext);

    BOOST_CHECK(sig.Complete());
    BOOST_CHECK(sig.Wait(WaitOptions{}) == WaitStatus::InvalidContext);
}

/* C-04：等待者被 Stop 强制回收后，晚到 Complete 只记录完成、
 * 不访问已注销等待者、不 crash；Stop 后不再允许新建信号 */
BOOST_AUTO_TEST_CASE(t_complete_after_stop)
{
    g_old_gen_signal = std::make_shared<CompletionSignal>();
    bbt::core::thread::CountDownLatch entered{1};
    std::atomic<Coroutine*> co_p{nullptr};

    bbtco [&]() {
        co_p.store(g_bbt_tls_coroutine_co);
        entered.Down();
        /* 无期限等待：协程将被 Stop 回收，Wait 不会返回 */
        g_old_gen_signal->Wait(WaitOptions{});
    };

    entered.Wait();
    BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));

    g_scheduler->Stop();
    g_started.store(false);

    /* 协程对象已销毁；Complete 只能记录完成 */
    BOOST_CHECK(g_old_gen_signal->Complete());
    BOOST_CHECK(!g_old_gen_signal->Complete());

    /* 已开始 Stop 后不存在可归属代际：新建信号拒绝 */
    BOOST_CHECK_THROW(CompletionSignal another, std::logic_error);
}

/* C-05：旧代际信号在新代际中 Wait → RuntimeUnavailable（即便已完成）；
 * 新代际新建信号工作正常 */
BOOST_AUTO_TEST_CASE(t_restart_old_generation)
{
    g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    g_started.store(true);
    BOOST_REQUIRE_NE(CurrentRuntimeGeneration(), 0u);

    bbt::core::thread::CountDownLatch done{1};
    std::atomic<WaitStatus> st{};
    bbtco [&]() {
        st.store(g_old_gen_signal->Wait(WaitOptions{}));
        done.Down();
    };
    done.Wait();
    BOOST_CHECK(st.load() == WaitStatus::RuntimeUnavailable);

    CompletionSignal fresh;
    BOOST_CHECK(fresh.Complete());
    bbt::core::thread::CountDownLatch done2{1};
    std::atomic<WaitStatus> st2{};
    bbtco [&]() {
        st2.store(fresh.Wait(WaitOptions{}));
        done2.Down();
    };
    done2.Wait();
    BOOST_CHECK(st2.load() == WaitStatus::Completed);
}

/* Critical 回归（独立审查 Critical-1）：等待者退出时会无条件清空 m_wait_event，
 * 若此时已有后到等待者登记，后者的登记会被抹掉 → Complete 只置位不通知 → 后者丢
 * 通知并挂到自己的 deadline。
 *
 * 构造：A 挂起（无期限、可被取消），B 在另一 worker 上自旋重试 Wait（evA 未终态
 * 返回 AlreadyWaiting，不占等待位）。主线程取消 A 使 A 恢复；A 的退出清理与 B 的
 * 登记处于同一竞争窗口，多轮重复以覆盖该窗口。
 * 断言：无论竞争结果如何，B 都必须观察到 Completed（修复后恒成立；未修复时在
 * 窗口命中轮次会丢通知并返回 TimedOut，使用例失败）。
 */
BOOST_AUTO_TEST_CASE(t_late_waiter_survives_first_waiter_exit)
{
    for (int round = 0; round < 100; ++round) {
        CompletionSignal sig;
        CancellationSource source;

        std::atomic_bool a_done{false};
        std::atomic<WaitStatus> a_status{WaitStatus::InvalidContext};

        bbtco [&]() {
            a_status.store(sig.Wait(WaitOptions{Deadline::max(), source.Token()}));
            a_done.store(true);
        };

        std::atomic_int b_live_iters{0};
        std::atomic_bool b_done{false};
        std::atomic<WaitStatus> b_status{WaitStatus::InvalidContext};

        bbtco [&]() {
            WaitStatus st;
            int iters = 0;
            do {
                st = sig.Wait(WaitOptions{
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(500),
                    CancellationToken{}});
                b_live_iters.store(++iters);
            } while (st == WaitStatus::AlreadyWaiting);
            b_status.store(st);
            b_done.store(true);
        };

        /* 等 B 确实进入自旋（A 已在挂起、B 已在竞争窗口外），再结束 A 的等待 */
        BOOST_REQUIRE(WaitUntil([&]() { return b_live_iters.load() > 0; }, 5000));
        source.RequestCancel();

        BOOST_REQUIRE(WaitUntil([&]() { return a_done.load(); }, 5000));

        /* A 的退出清理已经发生；此后 Complete 必须仍能唤醒已登记的 B */
        BOOST_CHECK(sig.Complete());
        BOOST_REQUIRE(WaitUntil([&]() { return b_done.load(); }, 5000));

        BOOST_CHECK_MESSAGE(b_status.load() == WaitStatus::Completed,
            "round " << round << "：后到等待者丢了通知（b_status="
                     << static_cast<int>(b_status.load())
                     << ", b_live_iters=" << b_live_iters.load()
                     << ", a_status=" << static_cast<int>(a_status.load()) << ")");
    }
}

/* #347③：登记前完成不丢唤醒。
 * (a) 协程创建前已 Complete：入口完成检查直接决议 Completed；
 * (b) Complete 与 Wait 入口并发 200 轮：完成可能落在「完成检查 →
 *     事件登记」窗口内，由在途计数 + Complete 重试投递关闭，
 *     结果必须恒为 Completed。 */
BOOST_AUTO_TEST_CASE(t_complete_before_wait_no_lost_wakeup)
{
    {
        CompletionSignal sig;
        BOOST_CHECK(sig.Complete());

        bbt::core::thread::CountDownLatch done{1};
        std::atomic<WaitStatus> st{};
        bbtco [&]() {
            st.store(sig.Wait(WaitOptions{}));
            done.Down();
        };
        done.Wait();
        BOOST_CHECK(st.load() == WaitStatus::Completed);
    }

    for (int i = 0; i < 200; ++i) {
        CompletionSignal sig;
        bbt::core::thread::CountDownLatch done{1};
        std::atomic<WaitStatus> st{};

        bbtco [&]() {
            st.store(sig.Wait(WaitOptions{}));
            done.Down();
        };

        sig.Complete();
        BOOST_REQUIRE_MESSAGE(done.WaitTimeout(5000) == 0,
            "round " << i << "：等待未在有界时间内返回");
        BOOST_CHECK_MESSAGE(st.load() == WaitStatus::Completed,
            "round " << i << "：登记前完成丢了通知");
    }
}

/* #347③：超时先胜出、Complete 后到——结果固定 TimedOut 不被改判，
 * 唤醒掩码只含 TIMEOUT 位；完成状态保留，下一次 Wait 仍观察到 Completed */
BOOST_AUTO_TEST_CASE(t_timeout_wins_then_complete_stays_timedout)
{
    CompletionSignal sig;
    WaitOptions short_opt;
    short_opt.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);

    bbt::core::thread::CountDownLatch first_done{1}, done{1};
    std::atomic<WaitStatus> st1{}, st2{};
    std::atomic<int> mask1{-1};

    bbtco [&]() {
        st1.store(sig.Wait(short_opt));
        mask1.store(g_bbt_tls_coroutine_co->GetLastResumeEvent());
        first_done.Down();
        st2.store(sig.Wait(WaitOptions{}));
        done.Down();
    };

    first_done.Wait();
    BOOST_CHECK(st1.load() == WaitStatus::TimedOut);
    BOOST_CHECK(mask1.load() & POLL_EVENT_TIMEOUT);
    BOOST_CHECK(!(mask1.load() & POLL_EVENT_CANCELLED));
    BOOST_CHECK(!(mask1.load() & POLL_EVENT_CUSTOM));

    /* 晚到 Complete：st1 已决议不得改判；完成状态留给下一次 Wait */
    BOOST_CHECK(sig.Complete());
    done.Wait();
    BOOST_CHECK(st2.load() == WaitStatus::Completed);
}

/* #347③：取消 / 超时 / 完成在 CompletionSignal 上三向可区分，
 * 唤醒掩码互不混淆（对齐切片②掩码断言手法） */
BOOST_AUTO_TEST_CASE(t_cancel_distinct_from_timeout_and_complete)
{
    /* (a) 真超时 → TimedOut，掩码仅 TIMEOUT */
    {
        CompletionSignal sig;
        WaitOptions opt;
        opt.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);

        bbt::core::thread::CountDownLatch done{1};
        std::atomic<WaitStatus> st{};
        std::atomic<int> mask{-1};

        bbtco [&]() {
            st.store(sig.Wait(opt));
            mask.store(g_bbt_tls_coroutine_co->GetLastResumeEvent());
            done.Down();
        };
        done.Wait();
        BOOST_CHECK(st.load() == WaitStatus::TimedOut);
        BOOST_CHECK(mask.load() & POLL_EVENT_TIMEOUT);
        BOOST_CHECK(!(mask.load() & POLL_EVENT_CANCELLED));
        BOOST_CHECK(!(mask.load() & POLL_EVENT_CUSTOM));
    }

    /* (b) 协程级取消 → Cancelled，掩码仅 CANCELLED */
    {
        CompletionSignal sig;
        bbt::core::thread::CountDownLatch done{1};
        std::atomic<Coroutine*> co_p{nullptr};
        std::atomic<WaitStatus> st{};
        std::atomic<int> mask{-1};

        bbtco [&]() {
            co_p.store(g_bbt_tls_coroutine_co);
            st.store(sig.Wait(WaitOptions{}));
            mask.store(g_bbt_tls_coroutine_co->GetLastResumeEvent());
            done.Down();
        };
        BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
        co_p.load()->RequestCancel();
        done.Wait();
        BOOST_CHECK(st.load() == WaitStatus::Cancelled);
        BOOST_CHECK(mask.load() & POLL_EVENT_CANCELLED);
        BOOST_CHECK(!(mask.load() & POLL_EVENT_TIMEOUT));
        BOOST_CHECK(!(mask.load() & POLL_EVENT_CUSTOM));
    }

    /* (c) Complete → Completed，掩码仅 CUSTOM */
    {
        CompletionSignal sig;
        bbt::core::thread::CountDownLatch done{1};
        std::atomic<Coroutine*> co_p{nullptr};
        std::atomic<WaitStatus> st{};
        std::atomic<int> mask{-1};

        bbtco [&]() {
            co_p.store(g_bbt_tls_coroutine_co);
            st.store(sig.Wait(WaitOptions{}));
            mask.store(g_bbt_tls_coroutine_co->GetLastResumeEvent());
            done.Down();
        };
        BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
        BOOST_CHECK(sig.Complete());
        done.Wait();
        BOOST_CHECK(st.load() == WaitStatus::Completed);
        BOOST_CHECK(mask.load() & POLL_EVENT_CUSTOM);
        BOOST_CHECK(!(mask.load() & POLL_EVENT_CANCELLED));
        BOOST_CHECK(!(mask.load() & POLL_EVENT_TIMEOUT));
    }
}

/* #347③：旧订阅晚到不污染下一轮等待（对齐切片②同名场景）。
 * 第一轮带令牌等待被取消收场；等待返回后再次置位令牌（幂等，登记
 * 已随窄接口返回解绑，无回调可晚到）；同一已取消令牌的第二轮入口
 * 即 Cancelled；第三轮无令牌真实挂起，必须由 Complete 以 CUSTOM
 * 掩码收场——陈旧取消状态不得泄漏进不携带该令牌的等待。 */
BOOST_AUTO_TEST_CASE(t_late_subscription_after_wait_returns)
{
    CompletionSignal sig;
    CancellationSource source;

    WaitOptions opt1;
    opt1.cancel = source.Token();

    bbt::core::thread::CountDownLatch first_done{1}, entered3{1}, done{1};
    std::atomic<Coroutine*> co_p{nullptr};
    std::atomic<WaitStatus> st1{}, st2{}, st3{};
    std::atomic<int> mask1{-1}, mask3{-1};

    bbtco [&]() {
        co_p.store(g_bbt_tls_coroutine_co);
        st1.store(sig.Wait(opt1));
        mask1.store(g_bbt_tls_coroutine_co->GetLastResumeEvent());
        first_done.Down();
        /* 第二轮复用同一已取消令牌：入口预检直接 Cancelled，不挂起 */
        st2.store(sig.Wait(opt1));
        entered3.Down();
        /* 第三轮不携带令牌：若旧订阅残留，任何晚到回调都可能误伤本轮 */
        st3.store(sig.Wait(WaitOptions{}));
        mask3.store(g_bbt_tls_coroutine_co->GetLastResumeEvent());
        done.Down();
    };

    BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
    source.RequestCancel();
    first_done.Wait();
    BOOST_CHECK(st1.load() == WaitStatus::Cancelled);
    BOOST_CHECK(mask1.load() & POLL_EVENT_CANCELLED);

    /* 第一轮等待已退出后才到的「晚到」取消：登记已解绑，只更新令牌
     * 自身状态，不再产生任何触发 */
    source.RequestCancel();

    /* 等第三轮真实挂起后再 Complete：结果必须是 CUSTOM 掩码的
     * Completed，证明无令牌等待未被陈旧取消订阅污染 */
    entered3.Wait();
    BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
    BOOST_CHECK(sig.Complete());
    done.Wait();
    BOOST_CHECK(st2.load() == WaitStatus::Cancelled);
    BOOST_CHECK(st3.load() == WaitStatus::Completed);
    BOOST_CHECK(mask3.load() & POLL_EVENT_CUSTOM);
    BOOST_CHECK(!(mask3.load() & POLL_EVENT_CANCELLED));
    BOOST_CHECK(!(mask3.load() & POLL_EVENT_TIMEOUT));
}

/* #347③：Scheduler::Stop 不展开协程栈——挂起中的 Wait 永不返回；
 * CompletionSignal 跨挂起状态不持有底层事件对象（成员只剩标量与
 * CoWaiter 句柄），等待者被 DestroyParkedCoroutines 回收后，信号
 * 仍可记录完成、可安全析构，无 UAF。 */
BOOST_AUTO_TEST_CASE(t_wait_state_survives_stop_no_unwind)
{
    auto sig = std::make_shared<CompletionSignal>();
    bbt::core::thread::CountDownLatch entered{1};
    std::atomic<Coroutine*> co_p{nullptr};
    std::atomic_bool wait_returned{false};

    bbtco [&]() {
        co_p.store(g_bbt_tls_coroutine_co);
        entered.Down();
        sig->Wait(WaitOptions{});
        /* Stop 不展开栈：此行不可达，协程被 parked 回收而非恢复 */
        wait_returned.store(true);
    };

    entered.Wait();
    BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));

    g_scheduler->Stop();
    g_started.store(false);

    /* Wait 未返回证明协程未展开；晚到 Complete 只记录完成；
     * 信号析构释放 CoWaiter 与其持有的终态事件，必须安全无 crash */
    BOOST_CHECK(!wait_returned.load());
    BOOST_CHECK(sig->Complete());
    sig.reset();
    BOOST_CHECK_THROW(CompletionSignal another, std::logic_error);
}

BOOST_AUTO_TEST_CASE(t_end)
{
    if (g_started.exchange(false))
        g_scheduler->Stop();

    if (g_cfg.saved) {
        auto* cfg = g_bbt_coroutine_config.get();
        cfg->m_cfg_static_thread_num = g_cfg.threads;
        cfg->m_cfg_stack_protect = g_cfg.stack_protect;
        g_cfg.saved = false;
    }
}

BOOST_AUTO_TEST_SUITE_END()
