#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>

#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(CoroutineLifecycleStateTest)

BOOST_AUTO_TEST_CASE(t_coroutine_state_transition_resume_yield)
{
    Coroutine::Ptr coroutine = nullptr;
    std::atomic_int running_checks{0};

    coroutine = Coroutine::Create(4096, [&]() {
        BOOST_CHECK_EQUAL(coroutine->GetStatus(), CoroutineStatus::CO_RUNNING);
        running_checks.fetch_add(1);
        coroutine->Yield();
        BOOST_CHECK_EQUAL(coroutine->GetStatus(), CoroutineStatus::CO_RUNNING);
        running_checks.fetch_add(1);
    }, false);

    BOOST_CHECK_EQUAL(coroutine->GetStatus(), CoroutineStatus::CO_RUNNABLE);
    coroutine->Resume();
    BOOST_CHECK_EQUAL(coroutine->GetStatus(), CoroutineStatus::CO_SUSPEND);
    coroutine->Resume();
    BOOST_CHECK_EQUAL(coroutine->GetStatus(), CoroutineStatus::CO_FINAL);
    BOOST_CHECK_EQUAL(running_checks.load(), 2);

    delete coroutine;
}

BOOST_AUTO_TEST_CASE(t_yield_with_callback_failure_restores_running_state)
{
    Coroutine::Ptr coroutine = nullptr;
    std::atomic_int callback_result{0};
    std::atomic_int status_after_failure{0};

    coroutine = Coroutine::Create(4096, [&]() {
        callback_result.store(coroutine->YieldWithCallback([]() {
            return false;
        }));
        status_after_failure.store(static_cast<int>(coroutine->GetStatus()));
        coroutine->Yield();
    }, false);

    coroutine->Resume();

    BOOST_CHECK_EQUAL(callback_result.load(), -1);
    BOOST_CHECK_EQUAL(status_after_failure.load(), static_cast<int>(CoroutineStatus::CO_RUNNING));
    BOOST_CHECK_EQUAL(coroutine->GetStatus(), CoroutineStatus::CO_SUSPEND);

    coroutine->Resume();
    BOOST_CHECK_EQUAL(coroutine->GetStatus(), CoroutineStatus::CO_FINAL);
    delete coroutine;
}

BOOST_AUTO_TEST_SUITE_END()