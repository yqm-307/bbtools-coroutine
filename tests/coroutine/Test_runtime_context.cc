#include <bbt/coroutine/object/CoRuntimeOnly.hpp>

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Processer.hpp>

using namespace bbt::coroutine;

namespace {

struct OnlyApi : CoRuntimeOnly {
    void Enter()
    {
        RequireCoroutineContext();
        ++calls;
    }
    int calls{0};
};

static_assert(std::is_empty_v<CoRuntimeOnly>);
static_assert(!std::is_polymorphic_v<CoRuntimeOnly>);
static_assert(std::is_trivially_copyable_v<CoRuntimeOnly>);
static_assert(std::is_copy_constructible_v<CoRuntimeOnly>);
static_assert(std::is_move_constructible_v<CoRuntimeOnly>);
static_assert(std::is_copy_assignable_v<CoRuntimeOnly>);
static_assert(std::is_move_assignable_v<CoRuntimeOnly>);

bool RejectsNativeCall()
{
    try {
        RequireCoroutineContext();
    } catch (const std::logic_error&) {
        return true;
    }
    return false;
}

// 应用约定的普通函数，不是宏，不接管 main，也不启动 runtime。
bool bbtco_main()
{
    RequireCoroutineContext();
    return IsInCoroutineContext();
}

} // namespace

BOOST_AUTO_TEST_SUITE(context_no_create)

BOOST_AUTO_TEST_CASE(native_gate_does_not_create_tls_processer_or_runtime)
{
    const auto before = detail::Processer::Create()->GetId();
    bool unchanged = false;
    std::thread thread([&]() {
        const bool cold = detail::LocalThread::TryGetTLSInst() == nullptr;
        bool rejected = true;
        for (int i = 0; i < 8; ++i) {
            rejected = rejected && !IsInCoroutineContext() && !CoRuntimeOnly::IsInCoroutineContext();
            rejected = rejected && RejectsNativeCall();
        }
        OnlyApi object;
        OnlyApi copied = object;
        OnlyApi moved = std::move(copied);
        try {
            moved.Enter();
            rejected = false;
        } catch (const std::logic_error&) {
        }
        unchanged = cold && rejected && moved.calls == 0 && detail::LocalThread::TryGetTLSInst() == nullptr;
    });
    thread.join();
    const auto after = detail::Processer::Create()->GetId();
    BOOST_CHECK(unchanged);
    BOOST_CHECK_EQUAL(after - before, 1u);
    BOOST_CHECK(!g_scheduler->IsInitialized());
}

BOOST_AUTO_TEST_CASE(released_tls_owner_clears_the_read_only_observer)
{
    bool cleared = false;
    std::thread thread([&]() {
        auto& local = detail::LocalThread::GetTLSInst();
        const bool visible = detail::LocalThread::TryGetTLSInst() == local.get();
        local.reset();
        cleared = visible && detail::LocalThread::TryGetTLSInst() == nullptr && !IsInCoroutineContext()
                  && RejectsNativeCall();
        auto& recreated = detail::LocalThread::GetTLSInst();
        cleared = cleared && detail::LocalThread::TryGetTLSInst() == recreated.get();
    });
    thread.join();
    BOOST_CHECK(cleared);
    BOOST_CHECK(!g_scheduler->IsInitialized());
}

BOOST_AUTO_TEST_CASE(enabled_idle_thread_is_not_a_coroutine)
{
    bool rejected = false;
    std::thread thread([&]() {
        auto& local = detail::LocalThread::GetTLSInst();
        local->SetEnableUseCo(true);
        const bool no_processer = detail::Processer::TryGetLocalProcesser() == nullptr;
        rejected = no_processer && !IsInCoroutineContext() && RejectsNativeCall()
                   && detail::Processer::TryGetLocalProcesser() == nullptr;
        local->SetEnableUseCo(false);
    });
    thread.join();
    BOOST_CHECK(rejected);
    BOOST_CHECK(!g_scheduler->IsInitialized());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(context_runtime)

BOOST_AUTO_TEST_CASE(real_coroutine_gate_survives_yield_and_respects_local_enable)
{
    g_bbt_coroutine_config->m_cfg_static_thread_num = 2;
    g_scheduler->Start();
    BOOST_CHECK(!IsInCoroutineContext());
    BOOST_CHECK_THROW(CoRuntimeOnly::RequireCoroutineContext(), std::logic_error);

    auto done = std::make_shared<std::promise<bool>>();
    auto future = done->get_future();
    g_scheduler->RegistCoroutineTask(128 * 1024, [done]() {
        try {
            OnlyApi object;
            object.Enter();
            const auto id = GetLocalCoroutineId();
            bool valid = id != 0 && IsInCoroutineContext() && CoRuntimeOnly::IsInCoroutineContext();
            CoRuntimeOnly::RequireCoroutineContext();
            detail::LocalThread::GetTLSInst()->SetEnableUseCo(false);
            const bool disabled = !IsInCoroutineContext() && RejectsNativeCall();
            detail::LocalThread::GetTLSInst()->SetEnableUseCo(true);
            bbtco_yield;
            object.Enter();
            valid = valid && disabled && GetLocalCoroutineId() == id && CoRuntimeOnly::IsInCoroutineContext()
                    && object.calls == 2;
            done->set_value(valid);
        } catch (...) {
            detail::LocalThread::GetTLSInst()->SetEnableUseCo(true);
            done->set_exception(std::current_exception());
        }
    });
    BOOST_REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    BOOST_CHECK(future.get());

    auto main_done = std::make_shared<std::promise<bool>>();
    auto main_future = main_done->get_future();
    bbtco[main_done]()
    {
        try {
            main_done->set_value(bbtco_main());
        } catch (...) {
            main_done->set_exception(std::current_exception());
        }
    };
    BOOST_REQUIRE(main_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    BOOST_CHECK(main_future.get());
    BOOST_CHECK(!IsInCoroutineContext());
}

BOOST_AUTO_TEST_SUITE_END()
