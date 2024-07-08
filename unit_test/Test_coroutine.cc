#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/detail/Coroutine.hpp>

using namespace bbt::coroutine;
detail::Coroutine::SPtr current_coroutine = nullptr;

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

BOOST_AUTO_TEST_SUITE_END()