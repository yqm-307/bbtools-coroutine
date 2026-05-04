#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>
#include <bbt/coroutine/sync/CoRWMutex.hpp>

using namespace bbt::coroutine;

BOOST_AUTO_TEST_SUITE(SyncPrimitivesStressTest)

BOOST_AUTO_TEST_CASE(t_start_scheduler)
{
    g_scheduler->Start();
}

BOOST_AUTO_TEST_CASE(t_sync_wait_notify_stress)
{
    auto cond = sync::CoCond::Create();
    const int waiter_num = 64;
    std::atomic_int woke_num{0};
    bbt::core::thread::CountDownLatch latch{waiter_num};

    for (int i = 0; i < waiter_num; ++i) {
        bbtco [cond, &woke_num, &latch]() {
            BOOST_REQUIRE_EQUAL(cond->Wait(), 0);
            ++woke_num;
            latch.Down();
        };
    }

    bbtco [cond]() {
        bbtco_sleep(20);
        cond->NotifyAll();
    };

    latch.Wait();
    BOOST_CHECK_EQUAL(woke_num.load(), waiter_num);
}

BOOST_AUTO_TEST_CASE(t_chan_competition_stress)
{
    sync::Chan<int, 16> chan;
    const int writer_num = 16;
    const int write_per_writer = 64;
    const int total = writer_num * write_per_writer;
    std::atomic_int read_count{0};
    bbt::core::thread::CountDownLatch latch{writer_num + 1};

    for (int i = 0; i < writer_num; ++i) {
        bbtco [&chan, &latch, write_per_writer, i]() {
            for (int j = 0; j < write_per_writer; ++j) {
                BOOST_REQUIRE_EQUAL(chan.Write(i * write_per_writer + j), 0);
            }
            latch.Down();
        };
    }

    bbtco [&chan, &read_count, &latch, total]() {
        for (int i = 0; i < total; ++i) {
            int value = -1;
            BOOST_REQUIRE_EQUAL(chan.Read(value), 0);
            ++read_count;
        }
        latch.Down();
    };

    latch.Wait();
    BOOST_CHECK_EQUAL(read_count.load(), total);
}

BOOST_AUTO_TEST_CASE(t_mutex_rwmutex_long_run_stability)
{
    auto mutex = bbtco_make_comutex();
    auto rwlock = sync::CoRWMutex::Create();
    std::atomic_bool running{true};
    std::atomic_bool write_phase{false};
    std::atomic_int mutex_count{0};
    std::atomic_int read_count{0};
    std::atomic_int write_count{0};
    bbt::core::thread::CountDownLatch latch{4};

    bbtco [&]() {
        bbtco_sleep(150);
        running = false;
        latch.Down();
    };

    bbtco [mutex, &running, &mutex_count, &latch]() {
        while (running.load()) {
            mutex->Lock();
            ++mutex_count;
            mutex->UnLock();
            bbtco_yield;
        }
        latch.Down();
    };

    bbtco [rwlock, &running, &write_phase, &read_count, &latch]() {
        while (running.load()) {
            rwlock->RLock();
            BOOST_CHECK(!write_phase.load());
            ++read_count;
            rwlock->UnLock();
            bbtco_yield;
        }
        latch.Down();
    };

    bbtco [rwlock, &running, &write_phase, &write_count, &latch]() {
        while (running.load()) {
            rwlock->WLock();
            write_phase = true;
            ++write_count;
            write_phase = false;
            rwlock->UnLock();
            bbtco_yield;
        }
        latch.Down();
    };

    latch.Wait();
    BOOST_CHECK_GT(mutex_count.load(), 0);
    BOOST_CHECK_GT(read_count.load(), 0);
    BOOST_CHECK_GT(write_count.load(), 0);
}

BOOST_AUTO_TEST_CASE(t_stop_scheduler)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()
