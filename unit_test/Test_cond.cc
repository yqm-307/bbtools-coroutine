#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
using namespace bbt::coroutine;

#define PrintTime(flag) printf("标记点=[%s]   协程id=[%ld] 时间戳=[%ld]\n", flag, GetLocalCoroutineId(), bbt::core::clock::now<>().time_since_epoch().count());

BOOST_AUTO_TEST_SUITE(CoCondTest)

BOOST_AUTO_TEST_CASE(t_begin)
{
    g_scheduler->Start();
}

BOOST_AUTO_TEST_CASE(t_cond_multi)
{
    std::atomic_bool run_in_co{false};

    bbtco [&run_in_co](){
        auto co1 = sync::CoWaiter::Create();
        auto co2 = sync::CoWaiter::Create();
        Assert(co1 != nullptr && co2 != nullptr);

        bbtco [&run_in_co, co1, co2](){
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
    while (!bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(max_end_ts))
    {
        std::this_thread::sleep_for(bbt::core::clock::milliseconds(10));
    }
}

BOOST_AUTO_TEST_CASE(t_cond_wait_with_timeout)
{
    const int wait_ms = 200;
    int a = 0;
    bbtco [&](){
        auto cond = sync::CoWaiter::Create();
        auto begin = bbt::core::clock::gettime();
        cond->WaitWithTimeout(wait_ms);
        auto end = bbt::core::clock::gettime();
        a++;

        BOOST_CHECK_GE(end - begin, wait_ms);
    };

    auto max_end_ts = bbt::core::clock::nowAfter(bbt::core::clock::milliseconds(500));
    while (a <= 0 && !bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(max_end_ts))
        std::this_thread::sleep_for(bbt::core::clock::milliseconds(10));

    BOOST_CHECK(a == 1);
}

BOOST_AUTO_TEST_CASE(t_cond_wait)
{
    std::mutex lock;
    const int nmax_co = 1000;
    std::atomic_int ncount{0};
    bbt::core::thread::CountDownLatch l2{nmax_co};

    bbtco [&](){
        auto cond = sync::CoCond::Create();

        for (int i = 0; i < nmax_co; ++i) {
            bbtco [&](){
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

// =================== P1: WaitFor(ms) ===================

// WaitFor(ms) 超时后正常返回
BOOST_AUTO_TEST_CASE(t_waitfor_timeout)
{
    bbt::core::thread::CountDownLatch l{1};
    int result = -999;

    bbtco [&](){
        auto cond = sync::CoCond::Create();
        auto begin = bbt::core::clock::gettime_mono();
        result = cond->WaitFor(100);
        auto elapsed = bbt::core::clock::gettime_mono() - begin;

        BOOST_TEST(result != 0);     // 超时不应返回 0
        BOOST_TEST(elapsed >= 90);    // 至少等了 ~100ms
        BOOST_TEST(elapsed < 1000);   // 不应等太久
        l.Down();
    };

    l.Wait();
    BOOST_TEST(result != 0);
}

// WaitFor(ms) 在 NotifyOne 之前被唤醒
BOOST_AUTO_TEST_CASE(t_waitfor_notifyone)
{
    bbt::core::thread::CountDownLatch l{1};
    int result = -999;

    bbtco [&](){
        auto cond = sync::CoCond::Create();

        bbtco [cond, &result, &l](){
            auto begin = bbt::core::clock::gettime_mono();
            result = cond->WaitFor(500);
            auto elapsed = bbt::core::clock::gettime_mono() - begin;
            BOOST_TEST(elapsed < 400); // 应该在 500ms 前被唤醒
            l.Down();
        };

        bbtco_sleep(50);
        cond->NotifyOne();
    };

    l.Wait();
    BOOST_TEST(result == 0); // NotifyOne 唤醒，返回 0
}

// WaitFor(ms) 在 NotifyAll 之前被唤醒
BOOST_AUTO_TEST_CASE(t_waitfor_notifyall)
{
    const int n = 50;
    bbt::core::thread::CountDownLatch l{n};
    std::atomic_int woken{0};

    bbtco [&](){
        auto cond = sync::CoCond::Create();

        for (int i = 0; i < n; ++i) {
            bbtco [cond, &woken, &l](){
                int ret = cond->WaitFor(500);
                if (ret == 0) woken++;
                l.Down();
            };
        }

        bbtco_sleep(100);
        cond->NotifyAll();
    };

    l.Wait();
    BOOST_TEST(woken == n);
}

// =================== P1: NotifyOne 空队列 ===================

// NotifyOne 空队列返回 -1
BOOST_AUTO_TEST_CASE(t_notifyone_empty)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco [&](){
        auto cond = sync::CoCond::Create();
        int ret = cond->NotifyOne();
        BOOST_TEST(ret == -1); // 空队列返回 -1
        l.Down();
    };

    l.Wait();
}

// NotifyOne 后 WaitFor 仍可正常使用
BOOST_AUTO_TEST_CASE(t_notifyone_empty_then_use)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco [&](){
        auto cond = sync::CoCond::Create();

        // 空队列 NotifyOne 应该返回 -1
        int ret = cond->NotifyOne();
        BOOST_TEST(ret == -1);

        // 之后正常 WaitFor + NotifyOne 仍然工作
        bbtco [cond](){
            bbtco_sleep(50);
            cond->NotifyOne();
        };

        ret = cond->WaitFor(200);
        BOOST_TEST(ret == 0);
        l.Down();
    };

    l.Wait();
}

// Wait + NotifyOne 单协程
BOOST_AUTO_TEST_CASE(t_wait_notifyone_single)
{
    bbt::core::thread::CountDownLatch l{1};
    int woken = 0;

    bbtco [&](){
        auto cond = sync::CoCond::Create();

        bbtco [cond, &woken, &l](){
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

BOOST_AUTO_TEST_CASE(t_end)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()
