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

bbt_co_async(int) Suspend(int value, int& stages)
{
    return bbt_co_bind(
        [](int value, int& stages) -> int {
            ++stages; // 进入挂起
            bbtco_yield;
            ++stages; // yield 返回
            bbtco_sleep(1);
            ++stages; // sleep 返回
            const auto result = bbt_co_await Identity(value);
            return result;
        },
        value,
        std::ref(stages));
}

bbt_co_async(std::string) SuspendString(std::string value, int& stages)
{
    return bbt_co_bind(
        [](std::string value, int& stages) -> std::string {
            ++stages; // 进入挂起
            bbtco_yield;
            ++stages; // yield 返回
            bbtco_sleep(1);
            ++stages; // sleep 返回
            return value;
        },
        std::move(value),
        std::ref(stages));
}

bbt_co_async(int) Fail()
{
    return bbt_co_bind([]() -> int { throw std::runtime_error("async failure"); });
}

bbt_co_async(int) EagerValue(int value)
{
    bbt_co_return value;
}

bbt_co_async(int) EagerSequence(int value, int& steps)
{
    const auto first = bbt_co_await Identity(value);
    ++steps;
    const auto second = bbt_co_await Identity(first + 1);
    ++steps;
    bbt_co_return second + 1;
}

bbt_co_async(void) EagerMark(int& calls)
{
    ++calls;
    bbt_co_return_void;
}

bbt_co_async(int) EagerEarly(bool fail, int& awaited)
{
    if (fail) {
        // 带括号的旧写法保持兼容，语义与不带括号的写法一致。
        bbt_co_return (-1);
    }
    ++awaited;
    const auto value = bbt_co_await Identity(7);
    bbt_co_return (value);
}

bbt_co_async(std::string) EagerString(std::string value, int& stages)
{
    std::string local = std::move(value);
    const std::string suffix = bbt_co_await SuspendString(" suffix", stages);
    local += suffix;
    // 左值返回按拷贝保存，结果不依赖函数栈上的 local 寿命。
    bbt_co_return local;
}

bbt_co_async(int&) EagerRef(int& value)
{
    bbt_co_return_ref(value);
}

bbt_co_async(std::unique_ptr<int>) EagerUnique(int value, int& created)
{
    auto owned = bbt_co_await Identity(std::make_unique<int>(value));
    ++created;
    bbt_co_return std::move(owned);
}

// 赋值优先级必须接住整个三元表达式，而不是只绑定条件。
bbt_co_async(int) EagerConditional(bool choose_high, int high, int low)
{
    bbt_co_return choose_high ? high : low;
}

// 顶层逗号须用括号分组，括号内保持逗号表达式语义。
bbt_co_async(int) EagerCommaExpression(int value, int& steps)
{
    bbt_co_return (++steps, value + 1);
}

bbt_co_async(int) EagerFail()
{
    throw std::runtime_error("eager async failure");
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

// worker 只记录具名事实，由测试线程断言，避免在 worker 内依赖 Boost.Test 的线程模型。
struct SyntaxFacts {
    bool ran = false;

    // 顺序 async 函数在调用时执行，副作用必须在消费前快照，否则无法区分 eager 与惰性。
    int before_consume_calls = -1;
    int before_consume_early_awaited = -1;
    int before_consume_normal_awaited = -1;
    int before_consume_string_stages = -1;
    int before_consume_sequence_steps = -1;
    int before_consume_ref_value = -1;
    int before_consume_suspend_stages = -1;
    int before_consume_unique_created = -1;
    bool before_consume_fail_threw = false;

    int eager_value = 0;
    int eager_sequence = 0;
    int eager_conditional = 0;
    int eager_comma = 0;
    int eager_comma_steps = -1;
    int eager_early_failed = 0;
    int eager_early_normal = 0;
    std::string eager_string;
    int eager_ref_value = 0;
    bool eager_ref_aliased = false;

    int pair_first = 0;
    int pair_second = 0;
    int generic_first = 0;
    int generic_second = 0;
    int pointer_value = 0;
    int unique_value = 0;
    int after_consume_calls = -1;
    int after_consume_early_awaited = -1;
    int after_consume_normal_awaited = -1;
    int after_consume_string_stages = -1;
    int after_consume_sequence_steps = -1;
    int after_consume_unique_created = -1;
    int nested = 0;
    int member = 0;
    int lambda_value = 0;
    int borrowed_value = 0;
    bool borrow_aliased = false;
    bool const_borrow_aliased = false;
    int calls = 0;
    int args = 0;
    int counted = 0;
    int moved_value = 0;
    bool snapshot_deferred = false;
    int snapshot_value = 0;
    int snapshot_calls = 0;
    bool repeated_consumption_rejected = false;
    int suspend_stages = 0;
    int resumed = 0;
    bool propagated = false;
    unsigned coroutine_id = 0;
    bool coroutine_id_stable = false;
};

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

BOOST_AUTO_TEST_CASE(reject_eager_await_outside_coroutine)
{
    int steps = 0;
    BOOST_CHECK_THROW((void)EagerSequence(1, steps), std::logic_error);
    BOOST_CHECK_EQUAL(steps, 0);
}

BOOST_AUTO_TEST_CASE(values_templates_and_suspend_in_real_coroutine)
{
    g_bbt_coroutine_config->m_cfg_static_thread_num = 2;
    g_scheduler->Start();
    auto done = std::make_shared<std::promise<SyntaxFacts>>();
    auto future = done->get_future();

    // 指定栈大小的启动语法糖尚未提供，见 SyntaxMacro.hpp 的「已知缺口」。
    g_scheduler->RegistCoroutineTask(128 * 1024, [done]() {
        try {
            SyntaxFacts facts;
            const auto id = bbt::coroutine::GetLocalCoroutineId();
            facts.ran = true;

            auto eager_value = EagerValue(2);
            int sequence_steps = 0;
            auto eager_sequence = EagerSequence(3, sequence_steps);
            auto eager_conditional = EagerConditional(false, 30, -30);
            int comma_steps = 0;
            auto eager_comma = EagerCommaExpression(20, comma_steps);
            int eager_calls = 0;
            auto eager_void = EagerMark(eager_calls);
            int early_awaited = 0;
            auto eager_early_failed = EagerEarly(true, early_awaited);
            int normal_awaited = 0;
            auto eager_early_normal = EagerEarly(false, normal_awaited);
            int string_stages = 0;
            auto eager_string = EagerString("request", string_stages);
            int eager_ref_value = 10;
            auto eager_ref = EagerRef(eager_ref_value);
            int unique_created = 0;
            auto eager_unique = EagerUnique(13, unique_created);
            bool eager_threw = false;
            try {
                (void)EagerFail();
            } catch (const std::runtime_error& error) {
                eager_threw = std::string(error.what()) == "eager async failure";
            }

            facts.before_consume_calls = eager_calls;
            facts.before_consume_early_awaited = early_awaited;
            facts.before_consume_normal_awaited = normal_awaited;
            facts.before_consume_string_stages = string_stages;
            facts.before_consume_sequence_steps = sequence_steps;
            facts.before_consume_ref_value = eager_ref_value;
            facts.before_consume_fail_threw = eager_threw;

            int suspend_stages = 0;
            auto suspended = Suspend(11, suspend_stages);
            facts.before_consume_suspend_stages = suspend_stages;
            facts.before_consume_unique_created = unique_created;

            facts.eager_value = bbt_co_await std::move(eager_value);
            facts.eager_sequence = bbt_co_await std::move(eager_sequence);
            facts.eager_conditional = bbt_co_await std::move(eager_conditional);
            facts.eager_comma = bbt_co_await std::move(eager_comma);
            facts.eager_comma_steps = comma_steps;
            bbt_co_await std::move(eager_void);
            facts.eager_early_failed = bbt_co_await std::move(eager_early_failed);
            facts.eager_early_normal = bbt_co_await std::move(eager_early_normal);
            facts.eager_string = bbt_co_await std::move(eager_string);
            int& eager_ref_result = bbt_co_await std::move(eager_ref);
            eager_ref_result = 11;
            facts.eager_ref_value = eager_ref_value;
            facts.eager_ref_aliased = &eager_ref_result == &eager_ref_value;
            auto unique_owner = bbt_co_await std::move(eager_unique);
            facts.unique_value = unique_owner ? *unique_owner : -1;

            auto pair = bbt_co_await MakePair(3);
            facts.pair_first = pair.first;
            facts.pair_second = pair.second;
            auto generic = bbt_co_await TemplatePair<int>(4);
            facts.generic_first = generic.first;
            facts.generic_second = generic.second;
            auto ptr = bbt_co_await Identity(std::make_unique<int>(5));
            facts.pointer_value = ptr ? *ptr : -1;
            const auto nested_source = bbt_co_await Identity(6);
            facts.nested = bbt_co_await Identity(nested_source);
            facts.member = bbt_co_await Reader{}.Read<int>(7);
            facts.lambda_value = bbt_co_await bbt_co_bind([](int value) { return value; }, 8);
            int value = 9;
            int& ref = bbt_co_await Borrow(value);
            ref = 10;
            const int& const_ref = bbt_co_await Borrow(std::as_const(value));
            facts.borrowed_value = value;
            facts.borrow_aliased = &ref == &value;
            facts.const_borrow_aliased = &const_ref == &value;
            int calls = 0;
            bbt_co_await Mark(calls);
            int args = 0;
            facts.counted = bbt_co_await CountCall(++args, calls);
            facts.args = args;
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
            facts.snapshot_deferred = snapshot_calls == 0;
            facts.snapshot_value = bbt_co_await std::move(snapshot);
            facts.snapshot_calls = snapshot_calls;
            auto one_shot = CountCall(12, calls);
            facts.moved_value = bbt_co_await std::move(one_shot);
            facts.calls = calls;
            try {
                (void)(bbt_co_await std::move(one_shot));
            } catch (const std::logic_error& error) {
                facts.repeated_consumption_rejected = std::string(error.what()) == "async operation already consumed";
            }
            facts.resumed = bbt_co_await std::move(suspended);
            facts.suspend_stages = suspend_stages;
            try {
                (void)(bbt_co_await Fail());
            } catch (const std::runtime_error& error) {
                facts.propagated = std::string(error.what()) == "async failure";
            }
            facts.coroutine_id = id;
            facts.coroutine_id_stable = bbt::coroutine::GetLocalCoroutineId() == id;
            facts.after_consume_calls = eager_calls;
            facts.after_consume_early_awaited = early_awaited;
            facts.after_consume_normal_awaited = normal_awaited;
            facts.after_consume_string_stages = string_stages;
            facts.after_consume_sequence_steps = sequence_steps;
            facts.after_consume_unique_created = unique_created;
            done->set_value(std::move(facts));
        } catch (...) {
            done->set_exception(std::current_exception());
        }
    });

    BOOST_REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const SyntaxFacts facts = future.get();

    BOOST_CHECK(facts.ran);
    BOOST_CHECK_EQUAL(facts.before_consume_calls, 1);
    BOOST_CHECK_EQUAL(facts.before_consume_early_awaited, 0);
    BOOST_CHECK_EQUAL(facts.before_consume_normal_awaited, 1);
    BOOST_CHECK_EQUAL(facts.before_consume_string_stages, 3);
    BOOST_CHECK_EQUAL(facts.before_consume_sequence_steps, 2);
    BOOST_CHECK_EQUAL(facts.before_consume_ref_value, 10);
    BOOST_CHECK(facts.before_consume_fail_threw);
    BOOST_CHECK_EQUAL(facts.before_consume_suspend_stages, 0);
    BOOST_CHECK_EQUAL(facts.before_consume_unique_created, 1);

    BOOST_CHECK_EQUAL(facts.eager_value, 2);
    BOOST_CHECK_EQUAL(facts.eager_sequence, 5);
    BOOST_CHECK_EQUAL(facts.eager_conditional, -30);
    BOOST_CHECK_EQUAL(facts.eager_comma, 21);
    BOOST_CHECK_EQUAL(facts.eager_comma_steps, 1);
    BOOST_CHECK_EQUAL(facts.eager_early_failed, -1);
    BOOST_CHECK_EQUAL(facts.eager_early_normal, 7);
    BOOST_CHECK_EQUAL(facts.eager_string, "request suffix");
    BOOST_CHECK_EQUAL(facts.eager_ref_value, 11);
    BOOST_CHECK(facts.eager_ref_aliased);

    BOOST_CHECK_EQUAL(facts.pair_first, 3);
    BOOST_CHECK_EQUAL(facts.pair_second, 4);
    BOOST_CHECK_EQUAL(facts.generic_first, 4);
    BOOST_CHECK_EQUAL(facts.generic_second, 4);
    BOOST_CHECK_EQUAL(facts.pointer_value, 5);
    BOOST_CHECK_EQUAL(facts.unique_value, 13);
    BOOST_CHECK_EQUAL(facts.after_consume_calls, 1);
    BOOST_CHECK_EQUAL(facts.after_consume_early_awaited, 0);
    BOOST_CHECK_EQUAL(facts.after_consume_normal_awaited, 1);
    BOOST_CHECK_EQUAL(facts.after_consume_string_stages, 3);
    BOOST_CHECK_EQUAL(facts.after_consume_sequence_steps, 2);
    BOOST_CHECK_EQUAL(facts.after_consume_unique_created, 1);
    BOOST_CHECK_EQUAL(facts.nested, 6);
    BOOST_CHECK_EQUAL(facts.member, 7);
    BOOST_CHECK_EQUAL(facts.lambda_value, 8);
    BOOST_CHECK_EQUAL(facts.borrowed_value, 10);
    BOOST_CHECK(facts.borrow_aliased);
    BOOST_CHECK(facts.const_borrow_aliased);
    BOOST_CHECK_EQUAL(facts.calls, 3);
    BOOST_CHECK_EQUAL(facts.args, 1);
    BOOST_CHECK_EQUAL(facts.counted, 1);
    BOOST_CHECK_EQUAL(facts.moved_value, 12);
    BOOST_CHECK(facts.snapshot_deferred);
    BOOST_CHECK_EQUAL(facts.snapshot_value, 21);
    BOOST_CHECK_EQUAL(facts.snapshot_calls, 1);
    BOOST_CHECK(facts.repeated_consumption_rejected);
    BOOST_CHECK_EQUAL(facts.suspend_stages, 3);
    BOOST_CHECK_EQUAL(facts.resumed, 11);
    BOOST_CHECK(facts.propagated);
    BOOST_CHECK_NE(facts.coroutine_id, 0u);
    BOOST_CHECK(facts.coroutine_id_stable);
}

BOOST_AUTO_TEST_CASE(fire_runs_after_submission_without_await)
{
    auto finished = std::make_shared<std::promise<int>>();
    auto future = finished->get_future();
    auto calls = std::make_shared<std::atomic_int>(0);

    bbtco [finished, calls]() {
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
