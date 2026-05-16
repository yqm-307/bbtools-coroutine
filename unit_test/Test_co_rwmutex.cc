#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/coroutine.hpp>


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
        auto cocond = bbt::coroutine::sync::CoCond::Create();
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();
        bbtco [=, &succ](){
            rwlock->RLock();
            succ = true;
            cocond->Wait();
            rwlock->RUnLock();
        };

        bbtco_sleep(50);

        bbtco [=, &succ](){

            bbtco_sleep(50);
            rwlock->RLock();
            BOOST_ASSERT(succ == true);
            cocond->NotifyAll();
            rwlock->RUnLock();
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
                rwlock->RUnLock();
                bbtco_yield;
            }
        };

        bbtco_desc("reader2") [&]() {
            while (running) {
                rwlock->RLock();
                BOOST_ASSERT(in_writing == false);
                nread++;
                rwlock->RUnLock();
            }
            bbtco_yield;
        };

        bbtco_desc("writer") [&]() {
            while (running) {
                rwlock->WLock();
                in_writing = true;
                in_writing = false;
                nwrite++;
                rwlock->WUnLock();
            }
        };

        bbtco_sleep(1000);
        running = false;
        bbtco_sleep(100);
        l.Down();
    };

    l.Wait();
}

/* 测试写优先：有 writer 排队时，新 reader 不得获取锁 */
BOOST_AUTO_TEST_CASE(t_writer_priority)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco_desc("main") [&](){
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();
        std::atomic_int reader2_status{0};
        bbt::core::thread::CountDownLatch writer_done{1};

        bbtco_desc("reader1") [&]() {
            rwlock->RLock();
            bbtco_sleep(100);
            rwlock->RUnLock();
        };

        bbtco_desc("writer") [&]() {
            rwlock->WLock();
            bbtco_sleep(20);
            rwlock->WUnLock();
            writer_done.Down();
        };

        bbtco_desc("reader2") [rwlock, &reader2_status]() {
            bbtco_sleep(10);
            rwlock->RLock();
            reader2_status = 1;
            rwlock->RUnLock();
        };

        writer_done.Wait();
        bbtco_sleep(50);
        BOOST_ASSERT(reader2_status == 1);
        l.Down();
    };

    l.Wait();
}

/* 测试 TryRLock 非阻塞获取 */
BOOST_AUTO_TEST_CASE(t_try_rlock)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco_desc("main") [&](){
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();

        BOOST_ASSERT(rwlock->TryRLock() == 0);
        rwlock->RUnLock();

        rwlock->WLock();
        BOOST_ASSERT(rwlock->TryRLock() == -1);
        rwlock->WUnLock();

        l.Down();
    };

    l.Wait();
}

/* 测试 TryWLock 非阻塞获取 */
BOOST_AUTO_TEST_CASE(t_try_wlock)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco_desc("main") [&](){
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();

        BOOST_ASSERT(rwlock->TryWLock() == 0);
        rwlock->WUnLock();

        rwlock->RLock();
        BOOST_ASSERT(rwlock->TryWLock() == -1);
        rwlock->RUnLock();

        l.Down();
    };

    l.Wait();
}

/* 测试 TryRLock(ms) 在 writer 持有时超时 */
BOOST_AUTO_TEST_CASE(t_try_rlock_timeout)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco_desc("main") [&](){
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();

        rwlock->WLock();
        int ret = rwlock->TryRLock(50);
        BOOST_ASSERT(ret == 1);
        rwlock->WUnLock();

        BOOST_ASSERT(rwlock->TryRLock(100) == 0);
        rwlock->RUnLock();

        l.Down();
    };

    l.Wait();
}

/* 测试 TryWLock(ms) 在 reader 持有时超时 */
BOOST_AUTO_TEST_CASE(t_try_wlock_timeout)
{
    bbt::core::thread::CountDownLatch l{1};

    bbtco_desc("main") [&](){
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();

        rwlock->RLock();
        int ret = rwlock->TryWLock(50);
        BOOST_ASSERT(ret == 1);
        rwlock->RUnLock();

        BOOST_ASSERT(rwlock->TryWLock(100) == 0);
        rwlock->WUnLock();

        l.Down();
    };

    l.Wait();
}

/* 测试 timeout 后继续正常使用锁 */
BOOST_AUTO_TEST_CASE(t_timeout_recovery)
{
    bbt::core::thread::CountDownLatch l{1};
    std::atomic_int n{0};

    bbtco_desc("main") [&](){
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();

        rwlock->WLock();

        bbtco_desc("timeout_reader") [rwlock]() {
            int ret = rwlock->TryRLock(50);
            BOOST_ASSERT(ret == 1);
        };

        bbtco_sleep(100);
        rwlock->WUnLock();

        bbtco_sleep(50);

        bbtco_desc("later_reader") [rwlock, &n]() {
            rwlock->RLock();
            n++;
            rwlock->RUnLock();
        };

        bbtco_sleep(100);
        BOOST_ASSERT(n == 1);
        l.Down();
    };

    l.Wait();
}

/* 增强多 reader / 多 writer 压力测试 */
BOOST_AUTO_TEST_CASE(t_rwlock_multi_co_stress)
{
    std::atomic_bool running{true};
    bbt::core::thread::CountDownLatch l{1};
    std::atomic_int nread{0};
    std::atomic_int nwrite{0};

    bbtco_desc("main") [&](){
        std::atomic_int in_writing{0};
        std::atomic_int active_readers{0};
        auto rwlock = bbt::coroutine::sync::CoRWMutex::Create();

        for (int i = 0; i < 4; ++i) {
            bbtco [&]() {
                while (running) {
                    rwlock->RLock();
                    active_readers.fetch_add(1);
                    BOOST_ASSERT(in_writing == 0);
                    nread.fetch_add(1);
                    active_readers.fetch_sub(1);
                    rwlock->RUnLock();
                    bbtco_yield;
                }
            };
        }

        for (int i = 0; i < 3; ++i) {
            bbtco [&]() {
                while (running) {
                    rwlock->WLock();
                    int old = in_writing.fetch_add(1);
                    BOOST_ASSERT(old == 0);
                    BOOST_ASSERT(active_readers == 0);
                    nwrite.fetch_add(1);
                    in_writing.fetch_sub(1);
                    rwlock->WUnLock();
                    bbtco_yield;
                }
            };
        }

        bbtco_sleep(500);
        running = false;
        bbtco_sleep(100);
        BOOST_ASSERT(nread > 0);
        BOOST_ASSERT(nwrite > 0);
        l.Down();
    };

    l.Wait();
}

BOOST_AUTO_TEST_CASE(t_stop_scheduler)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()