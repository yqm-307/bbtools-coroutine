#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <bbt/core/thread/Lock.hpp>

BOOST_AUTO_TEST_SUITE(CountDownLatchTest)

/* Wait() 基本唤醒：count=1，另一线程 Down 后返回 */
BOOST_AUTO_TEST_CASE(t_wait_normal)
{
    bbt::core::thread::CountDownLatch latch{1};
    std::atomic_bool done{false};

    std::thread t([&]() {
        latch.Wait();
        done = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    BOOST_TEST(done.load() == false);

    latch.Down();
    t.join();
    BOOST_TEST(done.load() == true);
}

/* WaitTimeout：Down 发生在超时前返回 0 */
BOOST_AUTO_TEST_CASE(t_wait_timeout_woken)
{
    bbt::core::thread::CountDownLatch latch{1};

    std::thread t([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        latch.Down();
    });

    int ret = latch.WaitTimeout(5000);
    t.join();
    BOOST_TEST(ret == 0);
}

/* WaitTimeout：无 Down 时真实等待 timeout 毫秒后返回 -1（回归：
   此前实现漏加 timeout，恒立即返回 -1） */
BOOST_AUTO_TEST_CASE(t_wait_timeout_expire)
{
    bbt::core::thread::CountDownLatch latch{1};

    auto begin = std::chrono::steady_clock::now();
    int ret = latch.WaitTimeout(120);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin).count();

    BOOST_TEST(ret == -1);
    BOOST_TEST(elapsed_ms >= 100);
}

/* WaitTimeout：count 已归零时立即返回 0 */
BOOST_AUTO_TEST_CASE(t_wait_timeout_zero_count)
{
    bbt::core::thread::CountDownLatch latch{0};

    auto begin = std::chrono::steady_clock::now();
    int ret = latch.WaitTimeout(5000);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin).count();

    BOOST_TEST(ret == 0);
    BOOST_TEST(elapsed_ms < 1000);
}

/* 多 waiter 广播唤醒：count=N，全部 Down 后所有 waiter 返回 */
BOOST_AUTO_TEST_CASE(t_broadcast_multi_waiter)
{
    constexpr int kN = 8;
    bbt::core::thread::CountDownLatch latch{kN};
    std::atomic_int woken{0};
    std::vector<std::thread> waiters;

    for (int i = 0; i < kN; ++i) {
        waiters.emplace_back([&]() {
            latch.Wait();
            woken++;
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    BOOST_TEST(woken.load() == 0);

    for (int i = 0; i < kN; ++i)
        latch.Down();

    for (auto& t : waiters)
        t.join();

    BOOST_TEST(woken.load() == kN);
}

/* while 语义：部分 Down 不唤醒 waiter（spurious wakeup 安全） */
BOOST_AUTO_TEST_CASE(t_partial_down_not_wake)
{
    bbt::core::thread::CountDownLatch latch{2};
    std::atomic_bool done{false};

    std::thread t([&]() {
        latch.Wait();
        done = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    latch.Down();  // count 1 -> 0，不应唤醒（还差 1）

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    BOOST_TEST(done.load() == false);

    latch.Down();  // count 0 -> 唤醒
    t.join();
    BOOST_TEST(done.load() == true);
}

BOOST_AUTO_TEST_SUITE_END()
