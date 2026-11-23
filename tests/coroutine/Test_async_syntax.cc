#include <bbt/coroutine/syntax/Async.hpp>

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include <bbt/coroutine/coroutine.hpp>

namespace {

template <class T> bbt_co_async(T) Identity(T value)
{
    return bbt_co_bind([](T value) -> T { return std::move(value); }, std::move(value));
}

bbt_co_async(std::pair<int, int>) MakePair(int value)
{
    return bbt_co_bind([](int value) -> std::pair<int, int> { return std::make_pair(value, value + 1); }, value);
}

#define TEST_ASYNC_PAIR(T) bbt_co_async(std::pair<T, T>)

template <class T> TEST_ASYNC_PAIR(T) TemplatePair(T value)
{
    return bbt_co_bind([](T value) -> std::pair<T, T> { return std::make_pair(value, value); }, std::move(value));
}

#undef TEST_ASYNC_PAIR

struct Reader {
    template <class T> bbt_co_async(T) Read(T value) const
    {
        return bbt_co_bind([](T value) -> T { return std::move(value); }, std::move(value));
    }
};

template <class T> bbt_co_async(T&) Borrow(T& value)
{
    return bbt_co_bind([](T& value) -> T& { return value; }, std::ref(value));
}

bbt_co_async(void) Mark(int& calls)
{
    return bbt_co_bind([](int& calls) { ++calls; }, std::ref(calls));
}

bbt_co_async(int) CountCall(int value, int& calls)
{
    return bbt_co_bind(
        [](int value, int& calls) {
            ++calls;
            return value;
        },
        value,
        std::ref(calls));
}

bbt_co_async(int) Suspend(int value)
{
    return bbt_co_bind(
        [](int value) -> int {
            bbtco_yield;
            bbtco_sleep(1);
            return bbt_co_await Identity(value);
        },
        value);
}

bbt_co_async(int) Fail()
{
    return bbt_co_bind([]() -> int { throw std::runtime_error("async failure"); });
}

using IntResult = decltype(Identity(1));
using MoveResult = decltype(Identity(std::make_unique<int>(1)));
using RefResult = decltype(Borrow(std::declval<int&>()));
using ConstRefResult = decltype(Borrow(std::declval<const int&>()));

template <class T, class = void> struct CanAwait : std::false_type {};

template <class T> struct CanAwait<T, std::void_t<decltype(bbt_co_await std::declval<T>())>> : std::true_type {};

template <class T, class = void> struct CanFire : std::false_type {};

template <class T> struct CanFire<T, std::void_t<decltype(bbt_co_fire std::declval<T>())>> : std::true_type {};

struct TakeOwnership {
    int operator()(std::unique_ptr<int>&&) &&;
};

template <class, class F, class... Args> struct CanBind : std::false_type {};

template <class F, class... Args>
struct CanBind<std::void_t<decltype(bbt_co_bind(std::declval<F>(), std::declval<Args>()...))>, F, Args...>
    : std::true_type {};

static_assert(!std::is_convertible_v<IntResult, int>);
static_assert(!CanAwait<int>::value);
static_assert(!CanAwait<IntResult&>::value);
static_assert(CanAwait<IntResult&&>::value);
static_assert(CanFire<IntResult&&>::value);
static_assert(std::is_same_v<decltype(bbt_co_await std::declval<IntResult>()), int>);
static_assert(std::is_same_v<decltype(bbt_co_await std::declval<RefResult>()), int&>);
static_assert(std::is_same_v<decltype(bbt_co_await std::declval<ConstRefResult>()), const int&>);
static_assert(!std::is_constructible_v<ConstRefResult, int&&>);
static_assert(!std::is_constructible_v<ConstRefResult, double&>);
static_assert(!std::is_copy_constructible_v<MoveResult>);
static_assert(std::is_move_constructible_v<MoveResult>);
static_assert(!CanBind<void, TakeOwnership, std::unique_ptr<int>&>::value, "move-only lvalue argument cannot be saved");
static_assert(CanBind<void, TakeOwnership, std::unique_ptr<int>>::value, "move-only rvalue argument remains bindable");

} // namespace

BOOST_AUTO_TEST_CASE(reject_before_arguments_and_body)
{
    int args = 0;
    int calls = 0;
    BOOST_CHECK_THROW((bbt_co_await bbt_co_bind([](int, int&) {}, ++args, std::ref(calls))), std::logic_error);
    BOOST_CHECK_THROW((bbt_co_await bbt_co_bind([](int& calls) { ++calls; }, std::ref(calls))), std::logic_error);
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

    // 指定栈大小的启动语法糖尚未提供，见 SyntaxMacro.hpp 的「已知缺口」。
    g_scheduler->RegistCoroutineTask(128 * 1024, [done]() {
        try {
            const auto id = bbt::coroutine::GetLocalCoroutineId();
            auto pair = bbt_co_await MakePair(3);
            auto generic = bbt_co_await TemplatePair<int>(4);
            auto ptr = bbt_co_await Identity(std::make_unique<int>(5));
            auto nested = bbt_co_await Identity(bbt_co_await Identity(6));
            auto member = bbt_co_await Reader{}.Read<int>(7);
            auto lambda_value = bbt_co_await bbt_co_bind([](int value) { return value; }, 8);
            int value = 9;
            int& ref = bbt_co_await Borrow(value);
            ref = 10;
            const int& const_ref = bbt_co_await Borrow(std::as_const(value));
            int calls = 0;
            bbt_co_await Mark(calls);
            int args = 0;
            auto counted = bbt_co_await CountCall(++args, calls);
            int snapshot_source = 21;
            int snapshot_calls = 0;
            auto snapshot = bbt_co_bind(
                [](int value, int& calls) {
                    ++calls;
                    return value;
                },
                snapshot_source,
                std::ref(snapshot_calls));
            snapshot_source = 22;
            const bool snapshot_deferred = snapshot_calls == 0;
            const int snapshot_value = bbt_co_await std::move(snapshot);
            auto one_shot = CountCall(12, calls);
            const auto one_shot_value = bbt_co_await std::move(one_shot);
            bool repeated_consumption_rejected = false;
            try {
                (void)(bbt_co_await std::move(one_shot));
            } catch (const std::logic_error& error) {
                repeated_consumption_rejected = std::string(error.what()) == "async operation already consumed";
            }
            auto resumed = bbt_co_await Suspend(11);
            bool propagated = false;
            try {
                (void)(bbt_co_await Fail());
            } catch (const std::runtime_error& error) {
                propagated = std::string(error.what()) == "async failure";
            }
            done->set_value(
                pair.first == 3 && pair.second == 4 && generic.first == 4 && generic.second == 4 && *ptr == 5
                && nested == 6 && member == 7 && lambda_value == 8 && value == 10 && &ref == &value
                && &const_ref == &value && calls == 3 && args == 1 && counted == 1 && one_shot_value == 12
                && snapshot_deferred && snapshot_value == 21 && snapshot_calls == 1 && repeated_consumption_rejected
                && resumed == 11 && propagated && id != 0 && bbt::coroutine::GetLocalCoroutineId() == id);
        } catch (...) {
            done->set_exception(std::current_exception());
        }
    });
    BOOST_REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    BOOST_CHECK(future.get());
}

BOOST_AUTO_TEST_CASE(fire_runs_after_submission_without_await)
{
    auto finished = std::make_shared<std::promise<int>>();
    auto future = finished->get_future();
    auto calls = std::make_shared<std::atomic_int>(0);

    bbtco[finished, calls]()
    {
        bbt_co_fire bbt_co_bind([finished, calls] {
            const int count = calls->fetch_add(1, std::memory_order_acq_rel) + 1;
            if (count == 1)
                finished->set_value(count);
        });
    };

    BOOST_REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    BOOST_CHECK_EQUAL(future.get(), 1);
    BOOST_CHECK_EQUAL(calls->load(), 1);
}
