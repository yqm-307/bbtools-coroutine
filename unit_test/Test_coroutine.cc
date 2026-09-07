#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <stdexcept>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>

using namespace bbt::coroutine;
detail::Coroutine::Ptr current_coroutine = nullptr;

BOOST_AUTO_TEST_SUITE(CoroutineTest)

BOOST_AUTO_TEST_CASE(t_coroutine_run)
{
    std::atomic_int ncount = 0;

    std::atomic_int sign_value = 0;

    
    current_coroutine = bbt::coroutine::detail::Coroutine::Create(4096,
    [&sign_value,&ncount](){
        for (int i = 0; i < 10000; ++i)
        {
            BOOST_CHECK_EQUAL(sign_value, 1);
            sign_value--;
            ncount++;
            current_coroutine->Yield();
        }

    });
    for (int i = 0; i < 10000; ++i)
    {
        BOOST_CHECK_EQUAL(sign_value, 0);
        sign_value++;
        ncount++;
        current_coroutine->Resume();
    }

    BOOST_CHECK_EQUAL(ncount, 20000);


}

BOOST_AUTO_TEST_CASE(t_status_create_runnable)
{
    auto* co = detail::Coroutine::Create(4096, [](){});
    BOOST_CHECK_EQUAL(co->GetStatus(), detail::CoroutineStatus::CO_RUNNABLE);
    BOOST_CHECK_NE(co->GetId(), 0u);
    delete co;
}

BOOST_AUTO_TEST_CASE(t_status_resume_yield_final)
{
    detail::Coroutine::Ptr co = nullptr;
    int phase = 0;
    co = detail::Coroutine::Create(4096, [&](){
        BOOST_CHECK_EQUAL(co->GetStatus(), detail::CoroutineStatus::CO_RUNNING);
        phase = 1;
        co->Yield();
        BOOST_CHECK_EQUAL(co->GetStatus(), detail::CoroutineStatus::CO_RUNNING);
        phase = 2;
    });

    BOOST_CHECK_EQUAL(co->GetStatus(), detail::CoroutineStatus::CO_RUNNABLE);
    co->Resume();
    BOOST_CHECK_EQUAL(phase, 1);
    BOOST_CHECK_EQUAL(co->GetStatus(), detail::CoroutineStatus::CO_SUSPEND);
    co->Resume();
    BOOST_CHECK_EQUAL(phase, 2);
    BOOST_CHECK_EQUAL(co->GetStatus(), detail::CoroutineStatus::CO_FINAL);
    delete co;
}

BOOST_AUTO_TEST_CASE(t_status_throw_reaches_final)
{
    const auto before = g_bbt_coroutine_config->m_unhandled_exception_count.load();
    auto* co = detail::Coroutine::Create(4096, [](){
        throw std::runtime_error("status-machine panic");
    });

    co->Resume();

    BOOST_CHECK_EQUAL(co->GetStatus(), detail::CoroutineStatus::CO_FINAL);
    BOOST_CHECK_EQUAL(
        g_bbt_coroutine_config->m_unhandled_exception_count.load(),
        before + 1);
    delete co;
}

BOOST_AUTO_TEST_SUITE_END()