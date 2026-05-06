#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <string>

#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>

using namespace bbt::coroutine;

namespace
{

struct RuntimeConfigSnapshot
{
    size_t threads{0};
    size_t stack_size{0};
    bool stack_protect{false};
    size_t trace_history_limit{0};
};

class ScopedSchedulerRuntime
{
public:
    explicit ScopedSchedulerRuntime(size_t threads)
    {
        auto* cfg = g_bbt_coroutine_config.get();
        m_snapshot.threads = cfg->m_cfg_static_thread_num;
        m_snapshot.stack_size = cfg->m_cfg_stack_size;
        m_snapshot.stack_protect = cfg->m_cfg_stack_protect;
        m_snapshot.trace_history_limit = cfg->m_cfg_trace_history_limit;

        cfg->m_cfg_static_thread_num = threads;
        cfg->m_cfg_stack_size = 4096;
        cfg->m_cfg_stack_protect = false;
        cfg->m_cfg_trace_history_limit = 32;
        ResetCoroutineTraceFilter();
        g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    }

    ~ScopedSchedulerRuntime()
    {
        if (g_scheduler->IsRunning())
            g_scheduler->Stop();

        ResetCoroutineTraceFilter();
        auto* cfg = g_bbt_coroutine_config.get();
        cfg->m_cfg_static_thread_num = m_snapshot.threads;
        cfg->m_cfg_stack_size = m_snapshot.stack_size;
        cfg->m_cfg_stack_protect = m_snapshot.stack_protect;
        cfg->m_cfg_trace_history_limit = m_snapshot.trace_history_limit;
    }

private:
    RuntimeConfigSnapshot m_snapshot;
};

bool HasEventKind(const CoroutineTraceSnapshot& snapshot, TraceEventKind kind)
{
    for (const auto& event : snapshot.recent_events) {
        if (event.kind == kind)
            return true;
    }

    return false;
}

}

BOOST_AUTO_TEST_SUITE(CoroutineTraceTest)

BOOST_AUTO_TEST_CASE(t_trace_filter_by_desc_preserves_recent_events_after_finish)
{
    ScopedSchedulerRuntime runtime(1);

    CoroutineTraceFilter filter;
    filter.descs.push_back("trace.target");
    SetCoroutineTraceFilter(filter);

    bbt::core::thread::CountDownLatch latch{1};
    std::atomic<detail::CoroutineId> trace_id{0};

    bbtco_desc("trace.target") [&]() {
        trace_id.store(GetLocalCoroutineId());
        bbtco_yield;
        bbtco_sleep(10);
        latch.Down();
    };

    latch.Wait();

    auto snapshot = QueryCoroutineTrace(trace_id.load());
    BOOST_REQUIRE(snapshot.found);
    BOOST_CHECK_EQUAL(snapshot.meta.desc, "trace.target");
    BOOST_CHECK_EQUAL(snapshot.meta.id, trace_id.load());
    BOOST_CHECK_EQUAL(snapshot.meta.status, detail::CoroutineStatus::CO_FINAL);
    BOOST_CHECK(HasEventKind(snapshot, TraceEventKind::CREATE));
    BOOST_CHECK(HasEventKind(snapshot, TraceEventKind::ENQUEUE));
    BOOST_CHECK(HasEventKind(snapshot, TraceEventKind::RESUME));
    BOOST_CHECK(HasEventKind(snapshot, TraceEventKind::YIELD));
    BOOST_CHECK(HasEventKind(snapshot, TraceEventKind::EVENT_REGISTER));
    BOOST_CHECK(HasEventKind(snapshot, TraceEventKind::EVENT_TRIGGER));
    BOOST_CHECK(HasEventKind(snapshot, TraceEventKind::FINISH));
    BOOST_CHECK(HasEventKind(snapshot, TraceEventKind::TEARDOWN));
}

BOOST_AUTO_TEST_CASE(t_trace_filter_supports_desc_prefix_for_active_query)
{
    ScopedSchedulerRuntime runtime(1);

    CoroutineTraceFilter filter;
    filter.desc_prefixes.push_back("trace.worker");
    SetCoroutineTraceFilter(filter);

    bbt::core::thread::CountDownLatch ready{1};
    bbt::core::thread::CountDownLatch release{1};

    bbtco_desc("trace.worker.alpha") [&]() {
        ready.Down();
        release.Wait();
    };

    ready.Wait();

    auto matches = QueryActiveCoroutinesByDesc("trace.worker", true);
    BOOST_REQUIRE_EQUAL(matches.size(), 1);
    BOOST_CHECK_EQUAL(matches.front().desc, "trace.worker.alpha");
    BOOST_CHECK(matches.front().traced);

    release.Down();
}

BOOST_AUTO_TEST_CASE(t_trace_filter_excludes_unmatched_coroutines)
{
    ScopedSchedulerRuntime runtime(1);

    CoroutineTraceFilter filter;
    filter.ids.push_back(999999);
    SetCoroutineTraceFilter(filter);

    bbt::core::thread::CountDownLatch latch{1};
    std::atomic<detail::CoroutineId> trace_id{0};

    bbtco_desc("trace.unmatched") [&]() {
        trace_id.store(GetLocalCoroutineId());
        latch.Down();
    };

    latch.Wait();

    auto snapshot = QueryCoroutineTrace(trace_id.load());
    BOOST_CHECK(!snapshot.found);
}

BOOST_AUTO_TEST_CASE(t_trace_history_is_bounded_by_configured_limit)
{
    ScopedSchedulerRuntime runtime(1);

    auto* cfg = g_bbt_coroutine_config.get();
    cfg->m_cfg_trace_history_limit = 4;

    CoroutineTraceFilter filter;
    filter.descs.push_back("trace.bound");
    SetCoroutineTraceFilter(filter);

    bbt::core::thread::CountDownLatch latch{1};
    std::atomic<detail::CoroutineId> trace_id{0};

    bbtco_desc("trace.bound") [&]() {
        trace_id.store(GetLocalCoroutineId());
        for (int i = 0; i < 6; ++i)
            bbtco_yield;
        latch.Down();
    };

    latch.Wait();

    auto snapshot = QueryCoroutineTrace(trace_id.load());
    BOOST_REQUIRE(snapshot.found);
    BOOST_CHECK_LE(snapshot.recent_events.size(), 4);
}

BOOST_AUTO_TEST_SUITE_END()
