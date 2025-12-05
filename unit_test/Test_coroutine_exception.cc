#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <vector>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Define.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

namespace
{
struct ScopedExceptionHandler
{
    ExceptionHandleCallback previous;
    std::vector<std::string> messages;
    mutable std::mutex guard;
    std::function<void()> notify;

    explicit ScopedExceptionHandler(std::function<void()> on_notify = {}):
        notify(std::move(on_notify))
    {
        previous = g_bbt_coroutine_config->m_ext_coevent_exception_callback;
        g_bbt_coroutine_config->m_ext_coevent_exception_callback = [this](const bbt::core::errcode::IErrcode& err) {
            {
                std::lock_guard<std::mutex> lock(guard);
                messages.emplace_back(err.What());
            }

            if (notify)
                notify();

            if (previous)
                previous(err);
        };
    }

    ~ScopedExceptionHandler()
    {
        g_bbt_coroutine_config->m_ext_coevent_exception_callback = previous;
    }

    std::vector<std::string> Snapshot() const
    {
        std::lock_guard<std::mutex> lock(guard);
        return messages;
    }
};

struct ConfigSnapshot
{
    size_t threads{0};
    size_t stack_size{0};
    bool stack_protect{false};
    bool saved{false};
} g_cfg_snapshot;

std::atomic_bool g_scheduler_started{false};

} // namespace

BOOST_AUTO_TEST_SUITE(CoroutineExceptionTest)

BOOST_AUTO_TEST_CASE(t_begin)
{
    auto* cfg = g_bbt_coroutine_config.get();
    if (!g_cfg_snapshot.saved) {
        g_cfg_snapshot.threads = cfg->m_cfg_static_thread_num;
        g_cfg_snapshot.stack_size = cfg->m_cfg_stack_size;
        g_cfg_snapshot.stack_protect = cfg->m_cfg_stack_protect;
        g_cfg_snapshot.saved = true;
    }

    cfg->m_cfg_static_thread_num = 1;
    cfg->m_cfg_stack_size = 4096;
    cfg->m_cfg_stack_protect = false;

    g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    g_scheduler_started.store(true);
    BOOST_REQUIRE(g_scheduler->IsRunning());
}

BOOST_AUTO_TEST_CASE(t_coroutine_throw_without_handler)
{
    BOOST_REQUIRE(g_scheduler_started.load());

    std::promise<CoroutineStatus> status_promise;
    auto status_future = status_promise.get_future();
    std::atomic_bool status_set{false};

    ScopedExceptionHandler handler([&]() {
        if (status_set.exchange(true))
            return;

        auto co = g_bbt_tls_coroutine_co;
        if (co != nullptr)
            status_promise.set_value(co->GetStatus());
        else
            status_promise.set_value(CoroutineStatus::CO_DEFAULT);
    });

    g_scheduler->RegistCoroutineTask([]() {
        throw std::runtime_error("co panic");
    });

    auto wait_result = status_future.wait_for(std::chrono::seconds(1));
    BOOST_REQUIRE(wait_result == std::future_status::ready);

    auto status = status_future.get();
    auto messages = handler.Snapshot();

    BOOST_REQUIRE_EQUAL(messages.size(), 1u);
    BOOST_CHECK_EQUAL(messages.front(), std::string("co panic"));
    BOOST_CHECK_EQUAL(status, CoroutineStatus::CO_FINAL);
}

// BOOST_AUTO_TEST_CASE(t_coroutine_throw_without_handler_terminate)
// {
//     auto previous = g_bbt_coroutine_config->m_ext_coevent_exception_callback;
//     g_bbt_coroutine_config->m_ext_coevent_exception_callback = nullptr;

//     Coroutine::Ptr co = Coroutine::Create(4096, []() {
//         throw std::runtime_error("boom");
//     });

//     bool terminated = false;
//     try {
//         // 这里没法catch到异常，因为异常会在线程外抛出
//         co->Resume();
//     } catch (const std::exception& e) {
//         terminated = true;
//         BOOST_CHECK_EQUAL(std::string(e.what()), std::string("boom"));
//     }

//     g_bbt_coroutine_config->m_ext_coevent_exception_callback = previous;
//     BOOST_CHECK(terminated);
//     BOOST_CHECK_EQUAL(co->GetStatus(), CoroutineStatus::CO_FINAL);
// }

BOOST_AUTO_TEST_CASE(t_end)
{
    if (g_scheduler_started.exchange(false))
        g_scheduler->Stop();

    if (g_cfg_snapshot.saved) {
        auto* cfg = g_bbt_coroutine_config.get();
        cfg->m_cfg_static_thread_num = g_cfg_snapshot.threads;
        cfg->m_cfg_stack_size = g_cfg_snapshot.stack_size;
        cfg->m_cfg_stack_protect = g_cfg_snapshot.stack_protect;
        g_cfg_snapshot.saved = false;
    }
}

BOOST_AUTO_TEST_SUITE_END()
