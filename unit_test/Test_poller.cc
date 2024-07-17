#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>


using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(CoPollerTest)

BOOST_AUTO_TEST_CASE(t_poller_timeout_event_single)
{
    std::atomic_int count = 0;
    const int stack_size = 4096;
    const int timeout_ms = 1000;

    auto co_sptr = Coroutine::Create(stack_size, [](){});
    auto event = CoPollEvent::Create(co_sptr,[&](auto event, int, int){
        count++;
    });

    BOOST_ASSERT(event->InitFdEvent(-1, bbt::pollevent::EventOpt::TIMEOUT, timeout_ms) == 0);
    BOOST_ASSERT(event->Regist() == 0);

    while (count != 1)
    {
        CoPoller::GetInstance()->PollOnce();
        std::this_thread::sleep_for(bbt::clock::milliseconds(1));
    }
    BOOST_CHECK(count == 1);
}

BOOST_AUTO_TEST_CASE(t_poller_evnet_cancel)
{
    std::atomic_bool flag = false;
    const int stack_size = 4096;
    const int timeout_ms = 200;

    auto co_sptr = Coroutine::Create(stack_size, [](){});
    auto event = CoPollEvent::Create(co_sptr, [&](auto event, int, int){
        flag.exchange(true);
    });

    Assert(event->InitFdEvent(-1, bbt::pollevent::EventOpt::TIMEOUT, timeout_ms) == 0);
    Assert(event->Regist() == 0);

    event->UnRegist();

    sleep(1);
    BOOST_CHECK_EQUAL(flag, false);
}

BOOST_AUTO_TEST_CASE(t_poller_timerout_event_multi)
{
    std::atomic_int count = 0;
    std::vector<CoPollEvent::SPtr> events;

    /* 创建并注册1000个定时事件 */
    for (int i = 0; i < 1000; ++i)
    {
        auto co_sptr = Coroutine::Create(4096, [](){});
        auto event = CoPollEvent::Create(co_sptr, [&count, co_sptr](auto, int, int){
            count++;
        });

        /* 初始化并注册事件 */
        BOOST_ASSERT(event->InitFdEvent(-1, bbt::pollevent::EventOpt::TIMEOUT, 500) == 0);
        BOOST_ASSERT(event->Regist() == 0);
        events.push_back(event);
    }

    // 非阻塞情况下程序最多活10s
    auto max_end_ts = bbt::clock::nowAfter(bbt::clock::seconds(2));

    /* 开始轮询，探测完成的事件并回调通知到协程事件完成 */
    while (!bbt::clock::is_expired<bbt::clock::milliseconds>(max_end_ts))
    {
        CoPoller::GetInstance()->PollOnce();
        std::this_thread::sleep_for(bbt::clock::milliseconds(2));
    }

    /* 所有超时时间完成，开始判断是否达成预期 */
    BOOST_CHECK_EQUAL(count.load(), 1000);
}

BOOST_AUTO_TEST_CASE(t_poller_timerout_event_multi_thread)
{
    /* 多线程注册事件，单线程处理（reactor模型） */

    std::atomic_int                 count = 0;
    std::vector<std::thread*>       threads;
    const int                       m_timeout_ms = 500;

    std::vector<CoPollEvent::SPtr>  events;
    std::mutex                      events_mutex;

    for (int i = 0; i < 2; ++i)
    {
        auto t = new std::thread([&](){
            for (int i = 0; i < 5000; ++i)
            {
                auto co_sptr = Coroutine::Create(4096, [](){}, false);
                auto event = CoPollEvent::Create(co_sptr, [&count, co_sptr](auto, int, int){
                    count++;
                });

                BOOST_ASSERT(event->InitFdEvent(-1, bbt::pollevent::EventOpt::TIMEOUT, m_timeout_ms) == 0);
                BOOST_ASSERT(event->Regist() == 0);

                events_mutex.lock();
                events.push_back(event);
                events_mutex.unlock();
            }
        });
        threads.push_back(t);
    }

    // 非阻塞情况下程序最多活10s
    auto max_end_ts = bbt::clock::nowAfter(bbt::clock::seconds(2));

    /* 开始轮询，探测完成的事件并回调通知到协程事件完成 */
    while (!bbt::clock::is_expired<bbt::clock::milliseconds>(max_end_ts))
    {
        CoPoller::GetInstance()->PollOnce();
        std::this_thread::sleep_for(bbt::clock::microseconds(2));
    }

    for (auto&& item : threads)
    {
        if (item->joinable())
            item->join();
    }

    BOOST_CHECK_EQUAL(count, 10000);
}

BOOST_AUTO_TEST_SUITE_END()