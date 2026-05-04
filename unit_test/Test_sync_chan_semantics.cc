#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/Chan.hpp>

using namespace bbt::coroutine;

BOOST_AUTO_TEST_SUITE(SyncChanSemanticsTest)

BOOST_AUTO_TEST_CASE(t_start_scheduler)
{
    g_scheduler->Start();
}

BOOST_AUTO_TEST_CASE(t_try_write_timeout_does_not_enqueue_item)
{
    sync::Chan<int, 1> chan;
    std::atomic_int write_result{-2};
    bbt::core::thread::CountDownLatch latch{1};

    bbtco [&]() {
        BOOST_REQUIRE_EQUAL(chan.Write(1), 0);
        write_result = chan.TryWrite(2, 20);
        latch.Down();
    };

    latch.Wait();

    BOOST_CHECK_NE(write_result.load(), 0);

    int first = -1;
    int second = -1;
    BOOST_REQUIRE_EQUAL(chan.TryRead(first), 0);
    BOOST_CHECK_EQUAL(first, 1);
    BOOST_CHECK_NE(chan.TryRead(second), 0);
}

BOOST_AUTO_TEST_CASE(t_try_read_timeout_restores_reader_state)
{
    sync::Chan<int, 1> chan;
    std::atomic_int timeout_result{-2};
    std::atomic_int write_result{-2};
    std::atomic_int read_result{-2};
    std::atomic_int read_value{-1};
    bbt::core::thread::CountDownLatch latch{1};

    bbtco [&]() {
        int value = 0;
        timeout_result = chan.TryRead(value, 20);
        write_result = chan.TryWrite(7);
        read_result = chan.TryRead(value);
        if (read_result.load() == 0)
            read_value = value;
        latch.Down();
    };

    latch.Wait();

    BOOST_CHECK_NE(timeout_result.load(), 0);
    BOOST_REQUIRE_EQUAL(write_result.load(), 0);
    BOOST_REQUIRE_EQUAL(read_result.load(), 0);
    BOOST_CHECK_EQUAL(read_value.load(), 7);
}

BOOST_AUTO_TEST_CASE(t_unbuffered_write_fails_when_close_interrupts_wait)
{
    sync::Chan<int, 0> chan;
    std::atomic_int write_result{-2};
    std::atomic_int read_result{-2};
    std::atomic_int read_value{-1};
    bbt::core::thread::CountDownLatch latch{2};

    bbtco [&]() {
        write_result = chan.Write(9);
        latch.Down();
    };

    bbtco [&]() {
        bbtco_sleep(20);
        chan.Close();
        int value = 0;
        read_result = chan.Read(value);
        if (read_result.load() == 0)
            read_value = value;
        latch.Down();
    };

    latch.Wait();

    BOOST_CHECK_NE(write_result.load(), 0);
    BOOST_CHECK_NE(read_result.load(), 0);
    BOOST_CHECK_NE(read_value.load(), 9);
}

BOOST_AUTO_TEST_CASE(t_stop_scheduler)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()
