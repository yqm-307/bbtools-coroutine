#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/coroutine.hpp>

BOOST_AUTO_TEST_SUITE(CoRWMutexTest)

BOOST_AUTO_TEST_CASE(t_start_scheduler)
{
    g_scheduler->Start(true);
}

/* 测试读锁非阻塞 */
BOOST_AUTO_TEST_CASE(t_rlock_block)
{
    bbt::thread::CountDownLatch l{1};
    std::mutex lock;
    bool succ = false;

    bbtco [&](){
        auto cocond = bbt::coroutine::sync::CoCond::Create(lock);
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();
        bbtco [=, &succ](){
            rwlock->RLock();
            succ = true;
            cocond->Wait();
            rwlock->UnLock();
        };

        bbtco_sleep(50);

        bbtco [=, &succ](){

            bbtco_sleep(50);
            rwlock->RLock();
            BOOST_ASSERT(succ == true);
            cocond->NotifyAll();
            rwlock->UnLock();
        };

        cocond->Wait();
        l.Down();
    };
    
    sleep(1);
    l.Wait();
    // printf("xxx\n");
}

BOOST_AUTO_TEST_CASE(t_stop_scheduler)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()