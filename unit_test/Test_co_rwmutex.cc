#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/coroutine.hpp>

std::mutex lock;

BOOST_AUTO_TEST_SUITE(CoRWMutexTest)

BOOST_AUTO_TEST_CASE(t_start_scheduler)
{
    g_scheduler->Start();
}

/* 测试读锁非阻塞 */
BOOST_AUTO_TEST_CASE(t_rlock_block)
{
    bbt::core::thread::CountDownLatch l{1};
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

BOOST_AUTO_TEST_CASE(t_rlock_wlock_block)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco_desc("main co") [&](){
        int n = 0, m = 0;
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();
        bbtco [&](){
            rwlock->RLock();
            n++;
        };

        bbtco [&](){
            m++;
            rwlock->WLock();
            n++;
        };

        bbtco_sleep(100);
        BOOST_ASSERT(n == m && n == 1);
        l.Down();
    };

    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_rwlock_multi_co)
{
    std::atomic_bool running = true;
    bbt::core::thread::CountDownLatch l{1};
    int nwrite = 0;
    int nread = 0;

    bbtco_desc("main") [&](){
        std::atomic_bool in_writing{false};
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();
        bbtco_desc("reader1") [&]() {
            while (running) {
                rwlock->RLock();
                BOOST_ASSERT(in_writing == false);
                nread++;
                rwlock->UnLock();
            }
        };

        bbtco_desc("reader2") [&]() {
            while (running) {
                rwlock->RLock();
                BOOST_ASSERT(in_writing == false);
                nread++;
                rwlock->UnLock();
            }
        };

        bbtco_desc("writer") [&]() {
            while (running) {
                rwlock->WLock();
                in_writing = true;
                in_writing = false;
                nwrite++;
                rwlock->UnLock();
            }
        };

        bbtco_sleep(1000);
        running = false;
        bbtco_sleep(100);
        l.Down();
    };

    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_stop_scheduler)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()