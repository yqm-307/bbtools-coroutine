/**
 * @file Test_combined_wait.cc
 * @brief #346 切片验收：CoWaiter 组合等待最小稳定入口。
 *
 * 覆盖：FD 首胜（READ/WRITE）、Notify 首胜、timeout 首胜、
 * RequestCancel/令牌取消、park 前 Notify 防丢、无效 fd/options、
 * 重复等待（等待位占用/复用）、Stop parked 回收（复用既有 Stop 契约）。
 * 确定性手段与 Test_cowaiter_outcome 一致：以 CO_SUSPEND 为屏障，
 * 禁止 sleep 凑时序。
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/pollevent/Event.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/sync/Cancellation.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

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

bool CoroutineSuspended(const std::atomic<Coroutine*>& co)
{
    auto* p = co.load();
    return p != nullptr && p->GetStatus() == CoroutineStatus::CO_SUSPEND;
}

bool NotifyUntilSuccess(const sync::CoWaiter::SPtr& waiter, int budget_ms = 10000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    while (waiter->Notify() != 0) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

struct PipeFds
{
    int read{-1};
    int write{-1};
};

PipeFds MakePipe()
{
    int fds[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(pipe(fds), 0);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);
    return PipeFds{fds[0], fds[1]};
}

void CloseFd(int& fd)
{
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

void ClosePipe(PipeFds& pipe)
{
    CloseFd(pipe.read);
    CloseFd(pipe.write);
}

}

BOOST_AUTO_TEST_SUITE(CombinedWaitTest)

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

/* 1. FD 首胜：无 Notify、无超时竞争，fd 就绪即返回 FdReadable，掩码含可读位 */
BOOST_AUTO_TEST_CASE(t_fd_readable_first_win)
{
    auto waiter = sync::CoWaiter::Create();
    PipeFds pipe = MakePipe();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<sync::CombinedWaitStatus> st{};
    std::atomic<int> mask{-1};
    std::atomic<Coroutine*> co_p{nullptr};

    sync::CombinedWaitOptions opt;
    opt.want_readable = true;
    opt.fd = pipe.read;

    bbtco [&]() {
        co_p.store(g_bbt_tls_coroutine_co);
        st.store(waiter->Wait(opt));
        mask.store(g_bbt_tls_coroutine_co->GetLastResumeEvent());
        done.Down();
    };
    BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));

    BOOST_CHECK_EQUAL(::write(pipe.write, "x", 1), 1);
    done.Wait();
    ClosePipe(pipe);
    BOOST_CHECK(st.load() == sync::CombinedWaitStatus::FdReadable);
    /* fd 触发位沿用底层 EventOpt 数值交付（READABLE=0x02） */
    BOOST_CHECK(mask.load() & bbt::pollevent::EventOpt::READABLE);
}

/* 2. WRITEABLE 首胜：对打开的 pipe 写端，interest 即刻就绪 */
BOOST_AUTO_TEST_CASE(t_fd_writeable_first_win)
{
    int fds[2] = {-1, -1};
    BOOST_REQUIRE_EQUAL(pipe(fds), 0);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);
    auto waiter = sync::CoWaiter::Create();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<sync::CombinedWaitStatus> st{};
    std::atomic<Coroutine*> co_p{nullptr};

    sync::CombinedWaitOptions opt;
    opt.want_writeable = true;
    opt.fd = fds[1];

    bbtco [&]() {
        co_p.store(g_bbt_tls_coroutine_co);
        st.store(waiter->Wait(opt));
        done.Down();
    };
    BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
    done.Wait();
    ::close(fds[0]);
    ::close(fds[1]);
    BOOST_CHECK(st.load() == sync::CombinedWaitStatus::FdWriteable);
}

/* 3. Notify 首胜：fd 一直不就绪，Notify 到达返回 Completed */
BOOST_AUTO_TEST_CASE(t_notify_first_win)
{
    auto waiter = sync::CoWaiter::Create();
    PipeFds pipe = MakePipe();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<sync::CombinedWaitStatus> st{};
    std::atomic<Coroutine*> co_p{nullptr};

    sync::CombinedWaitOptions opt;
    opt.want_readable = true;
    opt.fd = pipe.read;

    bbtco [&]() {
        co_p.store(g_bbt_tls_coroutine_co);
        st.store(waiter->Wait(opt));
        done.Down();
    };
    BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
    BOOST_CHECK_EQUAL(waiter->Notify(), 0);
    done.Wait();
    ClosePipe(pipe);
    BOOST_CHECK(st.load() == sync::CombinedWaitStatus::Completed);
}

/* 4. timeout 首胜：fd 不就绪、无 Notify，到点返回 TimedOut */
BOOST_AUTO_TEST_CASE(t_timeout_first_win)
{
    auto waiter = sync::CoWaiter::Create();
    PipeFds pipe = MakePipe();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<sync::CombinedWaitStatus> st{};
    std::atomic<int> mask{-1};

    sync::CombinedWaitOptions opt;
    opt.want_readable = true;
    opt.fd = pipe.read;
    opt.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);

    bbtco [&]() {
        st.store(waiter->Wait(opt));
        mask.store(g_bbt_tls_coroutine_co->GetLastResumeEvent());
        done.Down();
    };
    done.Wait();
    ClosePipe(pipe);
    BOOST_CHECK(st.load() == sync::CombinedWaitStatus::TimedOut);
    BOOST_CHECK(mask.load() & POLL_EVENT_TIMEOUT);
}

/* 5. 协程级 RequestCancel 首胜 */
BOOST_AUTO_TEST_CASE(t_request_cancel_first_win)
{
    auto waiter = sync::CoWaiter::Create();
    PipeFds pipe = MakePipe();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<sync::CombinedWaitStatus> st{};
    std::atomic<Coroutine*> co_p{nullptr};

    sync::CombinedWaitOptions opt;
    opt.want_readable = true;
    opt.fd = pipe.read;

    bbtco [&]() {
        co_p.store(g_bbt_tls_coroutine_co);
        st.store(waiter->Wait(opt));
        done.Down();
    };
    BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
    co_p.load()->RequestCancel();
    done.Wait();
    ClosePipe(pipe);
    BOOST_CHECK(st.load() == sync::CombinedWaitStatus::Cancelled);
}

/* 6. 取消令牌首胜：挂起中 RequestCancel → Cancelled；入口预取消不挂起 */
BOOST_AUTO_TEST_CASE(t_token_cancel_first_win)
{
    /* (a) 挂起中令牌取消 */
    {
        auto waiter = sync::CoWaiter::Create();
        CancellationSource source;
        PipeFds pipe = MakePipe();
        bbt::core::thread::CountDownLatch done{1};
        std::atomic<sync::CombinedWaitStatus> st{};
        std::atomic<Coroutine*> co_p{nullptr};

        sync::CombinedWaitOptions opt;
        opt.want_readable = true;
        opt.fd = pipe.read;
        opt.cancel = source.Token();

        bbtco [&]() {
            co_p.store(g_bbt_tls_coroutine_co);
            st.store(waiter->Wait(opt));
            done.Down();
        };
        BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
        source.RequestCancel();
        done.Wait();
        ClosePipe(pipe);
        BOOST_CHECK(st.load() == sync::CombinedWaitStatus::Cancelled);
    }

    /* (b) 入口已取消：立即返回，不发生挂起 */
    {
        auto waiter = sync::CoWaiter::Create();
        CancellationSource source;
        source.RequestCancel();
        PipeFds pipe = MakePipe();
        bbt::core::thread::CountDownLatch done{1};
        std::atomic<sync::CombinedWaitStatus> st{};

        sync::CombinedWaitOptions opt;
        opt.want_readable = true;
        opt.fd = pipe.read;
        opt.cancel = source.Token();

        bbtco [&]() {
            st.store(waiter->Wait(opt));
            done.Down();
        };
        done.Wait();
        ClosePipe(pipe);
        BOOST_CHECK(st.load() == sync::CombinedWaitStatus::Cancelled);
    }
}

/* 7. park 前 Notify 防丢：自旋 Notify 覆盖 INITED/ARMED/PARKED 各落点，
 * 决议恒为 Completed（无 fd interest 的纯信号等待也走组合入口） */
BOOST_AUTO_TEST_CASE(t_notify_before_park_no_lost_wakeup)
{
    for (int i = 0; i < 100; ++i) {
        auto waiter = sync::CoWaiter::Create();
        bbt::core::thread::CountDownLatch done{1};
        std::atomic<sync::CombinedWaitStatus> st{};

        bbtco [&]() {
            st.store(waiter->Wait(sync::CombinedWaitOptions{}));
            done.Down();
        };

        std::thread notifier([&]() { NotifyUntilSuccess(waiter); });
        BOOST_REQUIRE_MESSAGE(done.WaitTimeout(5000) == 0,
            "round " << i << "：等待未在有界时间内返回");
        notifier.join();
        BOOST_CHECK_MESSAGE(st.load() == sync::CombinedWaitStatus::Completed,
            "round " << i << "：park 前通知丢失");
    }
}

/* 8 的探针辅助：独立 waiter，避免污染其它用例 */
static sync::CombinedWaitStatus waiter_bad_probe(const sync::CombinedWaitOptions& opt)
{
    auto waiter = sync::CoWaiter::Create();
    return waiter->Wait(opt);
}

/* 8. 无效 options：fd<0 含 fd interest → InvalidOptions 不挂起 */
BOOST_AUTO_TEST_CASE(t_invalid_options_rejected)
{
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<sync::CombinedWaitStatus> st{};

    bbtco [&]() {
        sync::CombinedWaitOptions bad_fd;
        bad_fd.want_readable = true;    /* fd 缺省 -1 */
        st.store(waiter_bad_probe(bad_fd));
        done.Down();
    };
    done.Wait();
    BOOST_CHECK(st.load() == sync::CombinedWaitStatus::InvalidOptions);
}

/* 9. READABLE + WRITEABLE 可同时申请：底层按 OR interest 监听，
 * pipe 读端写入数据后，首个交付位应被判为 FdReadable，而不是 InvalidOptions。 */
BOOST_AUTO_TEST_CASE(t_read_write_interest_combined)
{
    auto waiter = sync::CoWaiter::Create();
    PipeFds pipe = MakePipe();
    bbt::core::thread::CountDownLatch done{1};
    std::atomic<sync::CombinedWaitStatus> st{};
    std::atomic<Coroutine*> co_p{nullptr};

    sync::CombinedWaitOptions opt;
    opt.want_readable = true;
    opt.want_writeable = true;
    opt.fd = pipe.read;

    bbtco [&]() {
        co_p.store(g_bbt_tls_coroutine_co);
        st.store(waiter->Wait(opt));
        done.Down();
    };
    BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(co_p); }));
    BOOST_REQUIRE_EQUAL(::write(pipe.write, "x", 1), 1);
    BOOST_REQUIRE_EQUAL(done.WaitTimeout(5000), 0);
    ClosePipe(pipe);
    BOOST_CHECK(st.load() == sync::CombinedWaitStatus::FdReadable);
}

/* 10. 重复等待：占用期第二个等待者 → AlreadyWaiting；前次终态后可复用 */
BOOST_AUTO_TEST_CASE(t_repeated_wait_slot_reuse)
{
    auto waiter = sync::CoWaiter::Create();
    bbt::core::thread::CountDownLatch first_done{1}, second_done{1}, third_done{1};
    std::atomic<sync::CombinedWaitStatus> st1{}, st2{}, st3{};
    std::atomic<Coroutine*> first_co{nullptr}, third_co{nullptr};

    sync::CombinedWaitOptions opt;
    opt.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);

    bbtco [&]() {
        first_co.store(g_bbt_tls_coroutine_co);
        st1.store(waiter->Wait(opt));
        first_done.Down();
    };

    BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(first_co); }));

    /* 第一个等待者仍占用事件槽时，另一协程必须立即得到 AlreadyWaiting。 */
    bbtco [&]() {
        st2.store(waiter->Wait(sync::CombinedWaitOptions{}));
        second_done.Down();
    };
    BOOST_REQUIRE_EQUAL(second_done.WaitTimeout(5000), 0);
    BOOST_CHECK(st2.load() == sync::CombinedWaitStatus::AlreadyWaiting);

    BOOST_REQUIRE_EQUAL(first_done.WaitTimeout(5000), 0);
    BOOST_CHECK(st1.load() == sync::CombinedWaitStatus::TimedOut);

    /* 前一等待恢复并出账后，同一个 waiter 可被新协程复用。 */
    bbtco [&]() {
        third_co.store(g_bbt_tls_coroutine_co);
        st3.store(waiter->Wait(sync::CombinedWaitOptions{}));
        third_done.Down();
    };
    BOOST_REQUIRE(WaitUntil([&]() { return CoroutineSuspended(third_co); }));
    BOOST_CHECK_EQUAL(waiter->Notify(), 0);
    BOOST_REQUIRE_EQUAL(third_done.WaitTimeout(5000), 0);
    BOOST_CHECK(st3.load() == sync::CombinedWaitStatus::Completed);
}

/* 10. Stop parked 回收（复用既有 Stop 契约）：组合等待挂起中的协程在
 * Scheduler::Stop 后被安全回收，进程不崩（本用例随测试进程结束验收） */
BOOST_AUTO_TEST_CASE(t_stop_recovers_parked_combined_wait)
{
    auto waiter = sync::CoWaiter::Create();
    PipeFds pipe = MakePipe();
    std::atomic_bool entered{false};

    bbtco [&]() {
        sync::CombinedWaitOptions opt;
        opt.want_readable = true;
        opt.fd = pipe.read;
        entered.store(true);
        waiter->Wait(opt);      /* 无 deadline：只能被 Stop 回收 */
    };
    BOOST_CHECK(WaitUntil([&]() { return entered.load(); }));

    g_scheduler->Stop();
    g_started.store(false);
    ClosePipe(pipe);
    /* 回收后允许重新 Start（复用既有 Stop 契约），供后续必要时扩展 */
    g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    g_started.store(true);
    BOOST_CHECK(g_scheduler->IsRunning());
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
