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
    auto begin_ts = bbt::clock::now<>().time_since_epoch().count();
    auto end_ts = begin_ts;
    auto co_sptr = Coroutine::Create(4096, [](){});
    auto event = CoPollEvent::Create(co_sptr, 1000, [&](Coroutine::SPtr co){
        count++;
        end_ts = bbt::clock::now<>().time_since_epoch().count();
    });

    event->RegistEvent();

    while (count != 1)
    {
        CoPoller::GetInstance()->PollOnce();
    }

    BOOST_CHECK_GE(end_ts - begin_ts, 1000);    // 超时时间不能提前
    BOOST_CHECK_LT(end_ts - begin_ts, 1010);    // 误差
}

BOOST_AUTO_TEST_CASE(t_poller_timerout_event_multi)
{
    std::atomic_int count = 0;
    std::map<CoroutineId, int64_t> m_begin_time_map;
    std::map<CoroutineId, int64_t> m_end_time_map;
    std::vector<CoPollEvent::SPtr> events;

    /* 创建并注册1000个定时事件 */
    for (int i = 0; i < 1000; ++i)
    {
        auto co_sptr = Coroutine::Create(4096, [](){});
        auto event = CoPollEvent::Create(co_sptr, 500, [&](Coroutine::SPtr co){
            count++;
            m_end_time_map.insert(std::make_pair(co->GetId(), bbt::clock::now<>().time_since_epoch().count()));
        });

        m_begin_time_map.insert(std::make_pair(co_sptr->GetId(), bbt::clock::now<>().time_since_epoch().count()));
        BOOST_ASSERT(event->RegistEvent() == 0);
        events.push_back(event);
    }

    // 非阻塞情况下程序最多活10s
    auto max_end_ts = bbt::clock::nowAfter(bbt::clock::seconds(10));

    /* 开始轮询，探测完成的事件并回调通知到协程事件完成 */
    while (m_begin_time_map.size() != m_end_time_map.size() && !bbt::clock::is_expired<bbt::clock::milliseconds>(max_end_ts))
    {
        BOOST_WARN_MESSAGE(!bbt::clock::is_expired<bbt::clock::milliseconds>(max_end_ts), "maybe lose event! " << "，begin: " << m_begin_time_map.size() << "，end:" << m_end_time_map.size());
        CoPoller::GetInstance()->PollOnce();
    }

    /* 所有超时时间完成，开始判断是否达成预期 */
    BOOST_CHECK_EQUAL(count.load(), 1000);

    for (auto&& item : m_begin_time_map)
    {
        auto begin_ts = item.second;
        auto end_ts = m_end_time_map[item.first];
        BOOST_CHECK_GE(end_ts - begin_ts, 500);    // 超时时间不能提前
        BOOST_CHECK_LT(end_ts - begin_ts, 510);    // 误差
    }
}

BOOST_AUTO_TEST_CASE(t_poller_timerout_event_multi_thread)
{
    /* 多线程注册事件，单线程处理（reactor模型） */

    std::atomic_int count = 0;
    std::vector<std::thread*> threads;
    std::map<CoroutineId, int64_t> m_begin_time_map;
    std::mutex                     m_begin_mutex;
    std::map<CoroutineId, int64_t> m_end_time_map;
    std::mutex                     m_end_mutex;

    std::vector<CoPollEvent::SPtr> events;
    std::mutex                     events_mutex;

    for (int i = 0; i < 10; ++i)
    {
        auto t = new std::thread([&](){
            for (int i = 0; i < 10000; ++i)
            {
                auto co_sptr = Coroutine::Create(4096, [](){}, false);
                auto event = CoPollEvent::Create(co_sptr, 500, [&](Coroutine::SPtr co){
                    count++;
                    m_end_mutex.lock();
                    m_end_time_map.insert(std::make_pair(co->GetId(), bbt::clock::now<>().time_since_epoch().count()));
                    m_end_mutex.unlock();
                });

                m_begin_mutex.lock();
                m_begin_time_map.insert(std::make_pair(co_sptr->GetId(), bbt::clock::now<>().time_since_epoch().count()));
                BOOST_ASSERT(event->RegistEvent() == 0);
                m_begin_mutex.unlock();


                events_mutex.lock();
                events.push_back(event);
                events_mutex.unlock();
            }
        });
        threads.push_back(t);
    }

    // 非阻塞情况下程序最多活10s
    auto max_end_ts = bbt::clock::nowAfter(bbt::clock::seconds(10));

    /* 开始轮询，探测完成的事件并回调通知到协程事件完成 */
    while (m_begin_time_map.size() != m_end_time_map.size() && !bbt::clock::is_expired<bbt::clock::milliseconds>(max_end_ts))
    {
        BOOST_WARN_MESSAGE(!bbt::clock::is_expired<bbt::clock::milliseconds>(max_end_ts), "maybe lose event! " << "，begin: " << m_begin_time_map.size() << "，end:" << m_end_time_map.size());
        CoPoller::GetInstance()->PollOnce();
    }

    for (auto&& item : threads)
    {
        if (item->joinable())
            item->join();
    }

    for (auto&& item : m_begin_time_map)
    {
        auto begin_ts = item.second;
        auto end_ts = m_end_time_map[item.first];
        BOOST_CHECK_GE(end_ts - begin_ts, 500);    // 超时时间不能提前
        BOOST_CHECK_LT(end_ts - begin_ts, 550);    // 误差
    }

    BOOST_CHECK_EQUAL(count, 100000);
}

BOOST_AUTO_TEST_SUITE_END()