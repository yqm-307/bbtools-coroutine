// 首先单独包含语法头，保证它不依赖伞头的 include 顺序。
#include <bbt/coroutine/syntax/Async.hpp>

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include <bbt/coroutine/coroutine.hpp>

namespace
{

template<class T>
bbt_co_async(T) Identity(T value)
{
    return std::move(value);
}

bbt_co_async(std::pair<int, int>) MakePair(int value)
{
    return std::make_pair(value, value + 1);
}

// 函数式宏展开后，模板参数中的逗号仍属于返回类型。
#define TEST_ASYNC_PAIR(T) bbt_co_async(std::pair<T, T>)
template<class T>
TEST_ASYNC_PAIR(T) TemplatePair(T value)
{
    return std::make_pair(value, value);
}
#undef TEST_ASYNC_PAIR

struct Reader
{
    template<class T>
    bbt_co_async(T) Read(T value) const { return std::move(value); }
};

template<class T>
bbt_co_async(T&) Borrow(T& value) { return value; }

bbt_co_async(void) Mark(int& calls)
{
    ++calls;
    return {};
}

bbt_co_async(int) CountCall(int value, int& calls)
{
    ++calls;
    return value;
}

bbt_co_async(int) Suspend(int value)
{
    bbtco_yield;
    bbtco_sleep(1);
    return bbt_co_await Identity(value);
}

bbt_co_async(int) Fail() { throw std::runtime_error("async failure"); }

using IntResult = decltype(Identity(1));
using MoveResult = decltype(Identity(std::make_unique<int>(1)));
using RefResult = decltype(Borrow(std::declval<int&>()));
using ConstRefResult = decltype(Borrow(std::declval<const int&>()));
using Await = bbt::coroutine::detail::AwaitHelper;

template<class T, class = void>
struct CanAwait : std::false_type {};
template<class T>
struct CanAwait<T, std::void_t<decltype(std::declval<Await>() ->* std::declval<T>())>>
    : std::true_type {};

static_assert(!std::is_convertible_v<IntResult, int>);
static_assert(!CanAwait<int>::value);
static_assert(!CanAwait<IntResult&>::value);
static_assert(CanAwait<IntResult&&>::value);
static_assert(std::is_same_v<decltype(std::declval<Await>() ->* std::declval<IntResult>()), int>);
static_assert(std::is_same_v<decltype(std::declval<Await>() ->* std::declval<RefResult>()), int&>);
static_assert(std::is_same_v<decltype(std::declval<Await>() ->* std::declval<ConstRefResult>()), const int&>);
static_assert(!std::is_constructible_v<ConstRefResult, int&&>);
static_assert(!std::is_constructible_v<ConstRefResult, double&>);
static_assert(!std::is_copy_constructible_v<MoveResult>);
static_assert(sizeof(IntResult) == sizeof(int));
static_assert(sizeof(MoveResult) == sizeof(std::unique_ptr<int>));

} // namespace

BOOST_AUTO_TEST_CASE(reject_before_arguments_and_body)
{
    int args = 0;
    int calls = 0;
    BOOST_CHECK_THROW((bbt_co_await CountCall(++args, calls)), std::logic_error);
    BOOST_CHECK_THROW((bbt_co_await Mark(calls)), std::logic_error);
    BOOST_CHECK_EQUAL(args, 0);
    BOOST_CHECK_EQUAL(calls, 0);
    BOOST_CHECK_EQUAL(bbt::coroutine::GetLocalCoroutineId(), 0u);
}

BOOST_AUTO_TEST_CASE(values_templates_and_suspend_in_real_coroutine)
{
    g_bbt_coroutine_config->m_cfg_static_thread_num = 2;
    g_scheduler->Start();
    auto done = std::make_shared<std::promise<bool>>();
    auto future = done->get_future();
    // 失败/超时也不把主线程栈借给仍在运行的协程。
    g_scheduler->RegistCoroutineTask(128 * 1024, [done]() {
        try
        {
            const auto id = bbt::coroutine::GetLocalCoroutineId();
            auto pair = bbt_co_await MakePair(3);
            auto generic = bbt_co_await TemplatePair<int>(4);
            auto ptr = bbt_co_await Identity(std::make_unique<int>(5));
            auto nested = bbt_co_await Identity(bbt_co_await Identity(6));
            auto member = bbt_co_await Reader{}.Read<int>(7);
            auto lambda = [](int value) -> bbt_co_async(int) { return value; };
            auto lambda_value = bbt_co_await lambda(8);
            int value = 9;
            int& ref = bbt_co_await Borrow(value);
            ref = 10;
            const int& const_ref = bbt_co_await Borrow(std::as_const(value));
            int calls = 0;
            bbt_co_await Mark(calls);
            int args = 0;
            auto counted = bbt_co_await CountCall(++args, calls);
            auto resumed = bbt_co_await Suspend(11);
            bool propagated = false;
            try { (void)(bbt_co_await Fail()); }
            catch (const std::runtime_error& error)
            {
                propagated = std::string(error.what()) == "async failure";
            }
            done->set_value(pair.first == 3 && pair.second == 4 &&
                generic.first == 4 && generic.second == 4 && *ptr == 5 &&
                nested == 6 && member == 7 && lambda_value == 8 && value == 10 &&
                &ref == &value && &const_ref == &value && calls == 2 && args == 1 &&
                counted == 1 && resumed == 11 && propagated && id != 0 &&
                bbt::coroutine::GetLocalCoroutineId() == id);
        }
        catch (...) { done->set_exception(std::current_exception()); }
    });
    BOOST_REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    BOOST_CHECK(future.get());
}
