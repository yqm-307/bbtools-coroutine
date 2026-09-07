#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/interface/ICoroutine.hpp>

using namespace bbt::coroutine::detail;

BOOST_AUTO_TEST_SUITE(CoroutineApiTest)

BOOST_AUTO_TEST_CASE(t_create_id_status_via_interface)
{
    Coroutine::Ptr co = Coroutine::Create(4096, [](){});
    ICoroutine* api = co;

    BOOST_CHECK_NE(api->GetId(), 0u);
    BOOST_CHECK_EQUAL(api->GetStatus(), CoroutineStatus::CO_RUNNABLE);
    BOOST_CHECK_EQUAL(api->GetId(), co->GetId());
    delete co;
}

BOOST_AUTO_TEST_CASE(t_resume_yield_final_via_interface)
{
    Coroutine::Ptr co = nullptr;
    co = Coroutine::Create(4096, [&](){
        BOOST_CHECK_EQUAL(co->GetStatus(), CoroutineStatus::CO_RUNNING);
        co->Yield();
        BOOST_CHECK_EQUAL(co->GetStatus(), CoroutineStatus::CO_RUNNING);
    });

    ICoroutine* api = co;
    api->Resume();
    BOOST_CHECK_EQUAL(api->GetStatus(), CoroutineStatus::CO_SUSPEND);
    api->Resume();
    BOOST_CHECK_EQUAL(api->GetStatus(), CoroutineStatus::CO_FINAL);
    delete co;
}

BOOST_AUTO_TEST_SUITE_END()
