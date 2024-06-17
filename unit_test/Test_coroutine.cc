#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/detail/Coroutine.hpp>

using namespace bbt::coroutine;
detail::Coroutine::SPtr current_coroutine = nullptr;

BOOST_AUTO_TEST_SUITE(CoroutineTest)

BOOST_AUTO_TEST_CASE(t_coroutine_run)
{
    
    current_coroutine = bbt::coroutine::detail::Coroutine::Create(4096,
    [](){
        printf("start\n");
        printf("coroutine point 1\n");
        current_coroutine->Yield();
        printf("coroutine point 2\n");
        current_coroutine->Yield();
        printf("coroutine point 3\n");

    });

    printf("main point 1\n");
    current_coroutine->Resume();
    printf("main point 2\n");
    current_coroutine->Resume();
    printf("main point 3\n");
    current_coroutine->Resume();

}

BOOST_AUTO_TEST_SUITE_END()