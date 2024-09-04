#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/coroutine.hpp>

BOOST_AUTO_TEST_SUITE(TestDefer)

BOOST_AUTO_TEST_CASE(t_defer)
{
    int i = 0;

    bbtco_defer {
        BOOST_CHECK(i == 0);
    };

    bbtco_defer {
        BOOST_CHECK(i == 2);
        i = 0;
    };
    
    bbtco_defer {
        BOOST_CHECK(i == 1);
        i = 2;
    };

    bbtco_defer {
        BOOST_CHECK(i == 0);
        i = 1;
    };

}

BOOST_AUTO_TEST_SUITE_END()