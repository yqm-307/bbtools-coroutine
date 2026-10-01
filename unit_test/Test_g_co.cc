#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>
#include <mutex>

#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/coroutine.hpp>

/* 进程寿命模型：runtime 只初始化一次，重复 Start 抛 std::logic_error。
 * 每个测试文件就是一个可执行，这里把用例内的 Start() 收敛为进程内一次初始化。 */
namespace
{
void EnsureRuntime()
{
    static std::once_flag once;
    std::call_once(once, [](){
        bbt::coroutine::detail::Scheduler::GetInstance()->Start();
    });
}
}
BOOST_AUTO_TEST_SUITE(CoroutineTest)

BOOST_AUTO_TEST_CASE(t_multi_coroutine)
{
    std::atomic_int ncount = 0;
    const int nco = 100000;
    EnsureRuntime();

    for (int i = 0; i < nco; ++i)
    {
        bool succ = false;
        while (!succ)
        {
            bbtco_noexcept(&succ) [&](){
                ncount++;
            };
        }
    }

    std::this_thread::sleep_for(bbt::core::clock::milliseconds(3000));
    BOOST_CHECK_EQUAL(ncount, nco);
}

BOOST_AUTO_TEST_SUITE_END()