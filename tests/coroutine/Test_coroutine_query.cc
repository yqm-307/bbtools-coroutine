#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <array>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <unistd.h>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Processer.hpp>

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;

namespace
{
// 本进程不启动 runtime；新线程逐个 join，ID 增量只包含显式探针和被测查询。
void CheckColdQuery(bool enabled, bool stack_query)
{
    const auto before = Processer::Create()->GetId();
    bool zero = true;
    std::thread thread([&]() {
        LocalThread::GetTLSInst()->SetEnableUseCo(enabled);
        for (int i = 0; i < 8; ++i)
        {
            if (stack_query)
                zero = zero && GetLocalCoroutineStackSize() == 0;
            else
                zero = zero && GetLocalCoroutineId() == 0;
        }
    });
    thread.join();
    const auto after = Processer::Create()->GetId();
    BOOST_CHECK(zero);
    BOOST_CHECK_EQUAL(after - before, 1u);
    BOOST_CHECK(!g_scheduler->IsInitialized());
}

struct Sample
{
    CoroutineId actual_id{0};
    CoroutineId queried_id{0};
    size_t actual_stack{0};
    size_t queried_stack{0};
    bool disabled_zero{false};
};
}

BOOST_AUTO_TEST_SUITE(query_no_create)
BOOST_AUTO_TEST_CASE(id_disabled) { CheckColdQuery(false, false); }
BOOST_AUTO_TEST_CASE(id_enabled) { CheckColdQuery(true, false); }
BOOST_AUTO_TEST_CASE(stack_disabled) { CheckColdQuery(false, true); }
BOOST_AUTO_TEST_CASE(stack_enabled) { CheckColdQuery(true, true); }

BOOST_AUTO_TEST_CASE(existing_idle_and_creating_entry)
{
    const auto before = Processer::Create()->GetId();
    bool same = false;
    bool zero = false;
    bool gate = false;
    std::thread thread([&]() {
        auto local = LocalThread::GetTLSInst().get();
        // 原创建型入口仍须在首次调用创建，之后复用同一 TLS 实例。
        auto processer = Processer::GetLocalProcesser();
        same = processer != nullptr && processer == Processer::GetLocalProcesser();
        local->SetEnableUseCo(true);
        same = same && local->GetProcesser() == processer;
        zero = GetLocalCoroutineId() == 0 && GetLocalCoroutineStackSize() == 0;
        local->SetEnableUseCo(false);
        gate = local->GetProcesser() == nullptr && GetLocalCoroutineId() == 0 &&
               GetLocalCoroutineStackSize() == 0;
    });
    thread.join();
    const auto after = Processer::Create()->GetId();
    BOOST_CHECK(same);
    BOOST_CHECK(zero);
    BOOST_CHECK(gate);
    BOOST_CHECK_EQUAL(after - before, 2u);
    BOOST_CHECK(!g_scheduler->IsInitialized());
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(query_runtime)
BOOST_AUTO_TEST_CASE(current_coroutine_before_and_after_yield)
{
    g_bbt_coroutine_config->m_cfg_static_thread_num = 2;
    g_scheduler->Start();
    std::array<CoroutineId, 2> ids{};
    const std::array<size_t, 2> requests{{64 * 1024 + 1, 96 * 1024 + 1}};
    for (size_t task = 0; task < requests.size(); ++task)
    {
        auto result = std::make_shared<std::promise<std::array<Sample, 2>>>();
        auto future = result->get_future();
        g_scheduler->RegistCoroutineTask(requests[task], [result]() {
            std::array<Sample, 2> samples{};
            for (size_t i = 0; i < samples.size(); ++i)
            {
                auto current = Processer::GetLocalProcesser()->GetCurrentCoroutine();
                auto& sample = samples[i];
                if (current != nullptr)
                {
                    sample.actual_id = current->GetId();
                    sample.actual_stack = current->GetStackSize();
                }
                sample.queried_id = GetLocalCoroutineId();
                sample.queried_stack = GetLocalCoroutineStackSize();
                LocalThread::GetTLSInst()->SetEnableUseCo(false);
                sample.disabled_zero = GetLocalCoroutineId() == 0 &&
                                       GetLocalCoroutineStackSize() == 0;
                LocalThread::GetTLSInst()->SetEnableUseCo(true);
                if (i == 0)
                    bbtco_yield;
            }
            // 只捕获 shared_ptr；主线程超时退出也不留下对其栈变量的引用。
            result->set_value(samples);
        });
        BOOST_REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        const auto samples = future.get();
        const size_t page = static_cast<size_t>(getpagesize());
        const size_t expected_stack = requests[task] + (page - requests[task] % page) % page;
        for (const auto& sample : samples)
        {
            BOOST_CHECK_NE(sample.actual_id, 0u);
            BOOST_CHECK_EQUAL(sample.queried_id, sample.actual_id);
            BOOST_CHECK_EQUAL(sample.queried_stack, sample.actual_stack);
            BOOST_CHECK_EQUAL(sample.queried_stack, expected_stack);
            BOOST_CHECK(sample.disabled_zero);
        }
        BOOST_CHECK_EQUAL(samples[0].queried_id, samples[1].queried_id);
        ids[task] = samples[0].queried_id;
    }
    BOOST_CHECK_NE(ids[0], ids[1]);
}
BOOST_AUTO_TEST_SUITE_END()
