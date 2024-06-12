#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include "bbt/coroutine/detail/Stack.hpp"

BOOST_AUTO_TEST_SUITE(Coroutine_Stack)

BOOST_AUTO_TEST_CASE(t_coroutine_stack_create)
{
    auto single_stack = bbt::coroutine::detail::Stack(1024 * 4);

    std::vector<bbt::coroutine::detail::Stack> array;

    for (int i = 0; i < 1000; ++i)
    {
        array.push_back(bbt::coroutine::detail::Stack(1024*40));
    }
}

BOOST_AUTO_TEST_SUITE_END()