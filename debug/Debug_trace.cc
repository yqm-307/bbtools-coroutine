#include <iostream>

#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

int main()
{
    auto* cfg = g_bbt_coroutine_config.get();
    cfg->m_cfg_static_thread_num = 1;
    cfg->m_cfg_stack_size = 4096;
    cfg->m_cfg_stack_protect = false;

    CoroutineTraceFilter filter;
    filter.desc_prefixes.push_back("trace.demo");
    SetCoroutineTraceFilter(filter);

    g_scheduler->Start();

    bbt::core::thread::CountDownLatch latch{1};
    detail::CoroutineId target_id = 0;

    bbtco_desc("trace.demo.worker") [&]() {
        target_id = GetLocalCoroutineId();
        bbtco_yield;
        bbtco_sleep(10);
        latch.Down();
    };

    latch.Wait();
    std::cout << DumpCoroutineTrace(target_id) << std::endl;

    g_scheduler->Stop();
    return 0;
}
