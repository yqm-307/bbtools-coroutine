#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <limits>
#include <thread>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoRWMutex.hpp>

using namespace bbt::coroutine;

BOOST_AUTO_TEST_SUITE(SyncRWMutexFairnessTest)

BOOST_AUTO_TEST_CASE(t_start_scheduler)
{
    g_scheduler->Start();
}

BOOST_AUTO_TEST_CASE(t_writer_preferred_policy_blocks_late_readers)
{
    auto rwlock = sync::CoRWMutex::Create();
    std::atomic_bool first_reader_locked{false};
    std::atomic_int acquire_seq{0};
    std::atomic_int writer_order{0};
    std::atomic_int late_reader_order{0};
    bbt::core::thread::CountDownLatch latch{3};

    bbtco [rwlock, &first_reader_locked, &latch]() {
        rwlock->RLock();
        first_reader_locked = true;
        bbtco_sleep(80);
        rwlock->UnLock();
        latch.Down();
    };

    bbtco [rwlock, &first_reader_locked, &acquire_seq, &writer_order, &latch]() {
        while (!first_reader_locked.load()) {
            bbtco_sleep(1);
        }

        rwlock->WLock();
        writer_order = ++acquire_seq;
        bbtco_sleep(20);
        rwlock->UnLock();
        latch.Down();
    };

    bbtco [rwlock, &first_reader_locked, &acquire_seq, &late_reader_order, &latch]() {
        while (!first_reader_locked.load()) {
            bbtco_sleep(1);
        }

        bbtco_sleep(10);
        rwlock->RLock();
        late_reader_order = ++acquire_seq;
        rwlock->UnLock();
        latch.Down();
    };

    latch.Wait();

    BOOST_REQUIRE_GT(writer_order.load(), 0);
    BOOST_REQUIRE_GT(late_reader_order.load(), 0);
    BOOST_CHECK_LT(writer_order.load(), late_reader_order.load());
}

BOOST_AUTO_TEST_CASE(t_non_owner_unlock_does_not_release_writer_lock)
{
    auto rwlock = sync::CoRWMutex::Create();
    std::atomic_bool writer_locked{false};
    std::atomic_bool writer_released{false};
    std::atomic_bool reader_acquired_before_release{false};
    bbt::core::thread::CountDownLatch latch{3};

    bbtco [rwlock, &writer_locked, &writer_released, &latch]() {
        rwlock->WLock();
        writer_locked = true;
        bbtco_sleep(80);
        writer_released = true;
        rwlock->UnLock();
        latch.Down();
    };

    bbtco [rwlock, &writer_locked, &latch]() {
        while (!writer_locked.load()) {
            bbtco_sleep(1);
        }

        rwlock->UnLock();
        latch.Down();
    };

    bbtco [rwlock, &writer_locked, &writer_released, &reader_acquired_before_release, &latch]() {
        while (!writer_locked.load()) {
            bbtco_sleep(1);
        }

        bbtco_sleep(20);
        rwlock->RLock();
        if (!writer_released.load())
            reader_acquired_before_release = true;
        rwlock->UnLock();
        latch.Down();
    };

    latch.Wait();

    BOOST_CHECK(!reader_acquired_before_release.load());
}

BOOST_AUTO_TEST_CASE(t_non_owner_unlock_does_not_release_reader_lock)
{
    auto rwlock = sync::CoRWMutex::Create();
    std::atomic_bool reader_locked{false};
    std::atomic_bool reader_released{false};
    std::atomic_bool writer_acquired_before_release{false};
    bbt::core::thread::CountDownLatch latch{3};

    bbtco [rwlock, &reader_locked, &reader_released, &latch]() {
        rwlock->RLock();
        reader_locked = true;
        bbtco_sleep(80);
        reader_released = true;
        rwlock->UnLock();
        latch.Down();
    };

    bbtco [rwlock, &reader_locked, &latch]() {
        while (!reader_locked.load()) {
            bbtco_sleep(1);
        }

        rwlock->UnLock();
        latch.Down();
    };

    bbtco [rwlock, &reader_locked, &reader_released, &writer_acquired_before_release, &latch]() {
        while (!reader_locked.load()) {
            bbtco_sleep(1);
        }

        bbtco_sleep(20);
        rwlock->WLock();
        if (!reader_released.load())
            writer_acquired_before_release = true;
        rwlock->UnLock();
        latch.Down();
    };

    latch.Wait();

    BOOST_CHECK(!writer_acquired_before_release.load());
}

BOOST_AUTO_TEST_CASE(t_waiting_writer_stays_ahead_during_writer_handoff)
{
    constexpr int kRounds = 32;
    constexpr int kLateReadersPerRound = 8;

    for (int round = 0; round < kRounds; ++round) {
        auto rwlock = sync::CoRWMutex::Create();
        std::atomic_bool first_reader_locked{false};
        std::atomic_bool first_writer_acquired{false};
        std::atomic_bool release_late_readers{false};
        std::atomic_int acquire_seq{0};
        std::atomic_int second_writer_order{0};
        std::atomic_int earliest_late_reader_order{std::numeric_limits<int>::max()};
        bbt::core::thread::CountDownLatch latch{3 + kLateReadersPerRound};

        bbtco [rwlock, &first_reader_locked, &latch]() {
            rwlock->RLock();
            first_reader_locked = true;
            bbtco_sleep(60);
            rwlock->UnLock();
            latch.Down();
        };

        bbtco [rwlock, &first_reader_locked, &first_writer_acquired, &acquire_seq, &latch]() {
            while (!first_reader_locked.load()) {
                bbtco_sleep(1);
            }

            rwlock->WLock();
            first_writer_acquired = true;
            ++acquire_seq;
            bbtco_sleep(40);
            rwlock->UnLock();
            latch.Down();
        };

        bbtco [rwlock, &first_reader_locked, &second_writer_order, &acquire_seq, &latch]() {
            while (!first_reader_locked.load()) {
                bbtco_sleep(1);
            }

            bbtco_sleep(5);
            rwlock->WLock();
            second_writer_order = ++acquire_seq;
            bbtco_sleep(5);
            rwlock->UnLock();
            latch.Down();
        };

        for (int i = 0; i < kLateReadersPerRound; ++i) {
            bbtco [rwlock,
                   &first_writer_acquired,
                   &release_late_readers,
                   &earliest_late_reader_order,
                   &acquire_seq,
                   &latch]() {
                while (!first_writer_acquired.load()) {
                    bbtco_sleep(1);
                }

                while (!release_late_readers.load()) {
                    bbtco_sleep(1);
                }

                rwlock->RLock();

                int order = ++acquire_seq;
                int current = earliest_late_reader_order.load();
                while (order < current &&
                       !earliest_late_reader_order.compare_exchange_weak(current, order)) {
                }

                rwlock->UnLock();
                latch.Down();
            };
        }

        while (!first_writer_acquired.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        release_late_readers = true;
        latch.Wait();

        BOOST_REQUIRE_GT(second_writer_order.load(), 0);
        BOOST_CHECK_LT(second_writer_order.load(), earliest_late_reader_order.load());
    }
}

BOOST_AUTO_TEST_CASE(t_stop_scheduler)
{
    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()
