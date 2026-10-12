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

/* 显式栈大小现在是真值（原先被忽略、实际借全局默认 12 KiB）。本文件的协程在
 * 自身栈上执行 Boost.Test 断言与 std::runtime_error 抛出：断言失败路径的
 * vformat/日志缓冲需要远超 4 KiB 的栈（见 Context.cc 对 DebugPrint 栈占用的
 * 既有注释），4 KiB 会在真实尺寸生效后压爆协程栈。故重型 fixture 显式给 64 KiB；
 * 这属于“按真实需求给足栈”，不是把显式大小重新变成摆设。 */
static constexpr size_t kHeavyCoStackSize = 64 * 1024;

BOOST_AUTO_TEST_SUITE(CoroutineTest)

BOOST_AUTO_TEST_CASE(t_coroutine_run)
{
    std::atomic_int ncount = 0;

    std::atomic_int sign_value = 0;


    current_coroutine = bbt::coroutine::detail::Coroutine::Create(kHeavyCoStackSize,
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
    auto* co = detail::Coroutine::Create(kHeavyCoStackSize, [](){});
    BOOST_CHECK_EQUAL(co->GetStatus(), detail::CoroutineStatus::CO_RUNNABLE);
    BOOST_CHECK_NE(co->GetId(), 0u);
    delete co;
}

BOOST_AUTO_TEST_CASE(t_status_resume_yield_final)
{
    detail::Coroutine::Ptr co = nullptr;
    int phase = 0;
    co = detail::Coroutine::Create(kHeavyCoStackSize, [&](){
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
    auto* co = detail::Coroutine::Create(kHeavyCoStackSize, [](){
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