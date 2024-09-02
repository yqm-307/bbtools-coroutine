#include <unistd.h>
#include <fcntl.h>
#include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/utils/DebugPrint.hpp>

namespace bbt::coroutine::sync
{

using namespace bbt::coroutine::detail;

CoCond::SPtr CoCond::Create()
{
    return std::make_shared<CoCond>();
}


CoCond::CoCond():
    m_run_status(COND_FREE)
{
}

CoCond::~CoCond()
{
}

int CoCond::Wait()
{
    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "please use CoCond in coroutine!"); // 请在协程中使用CoCond
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "running a non-corourine!");      // 当前运行的非协程

    std::unique_lock<std::mutex> lock(m_co_event_mutex);

    /* 保证只有一个协程可以成功挂起 */
    if (m_run_status != COND_FREE)
        return -1;

    auto [regist_ret, id] = g_bbt_poller->RegistCustom<CoCond>(shared_from_this(), detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND);
    if (regist_ret != 0)
        return -1;

    m_co_event_id = id;
    m_run_status = COND_WAIT;

    /* 挂起当前协程并在挂起后解锁，保证事件触发在挂起协程后发生 */
    g_bbt_tls_coroutine_co->YieldWithCallback([&lock](){ 
        lock.unlock();
        return true;
    });

    /* 必须等待协程返回才可以释放cond */
    lock.lock();
    m_run_status = COND_FREE;
    m_co_event_id = 0;
    return 0;
}

int CoCond::WaitWithTimeout(int ms)
{
    int ret = 0;

    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "please use CoCond in coroutine!"); // 请在协程中使用CoCond
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "running a non-corourine!");      // 当前运行的非协程

    std::unique_lock<std::mutex> lock(m_co_event_mutex);

    /* 保证只有一个协程可以成功挂起 */
    if (m_run_status != COND_FREE)
        return -1;

    auto [regist_ret, id] = g_bbt_poller->RegistCustom<CoCond>(shared_from_this(), detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND, ms);
    if (regist_ret != 0)
        return -1;

    m_co_event_id = id;
    m_run_status = COND_WAIT;

    /* 挂起当前协程并在挂起后解锁，保证事件触发在挂起协程后发生 */
    g_bbt_tls_coroutine_co->YieldWithCallback([&lock](){ 
        lock.unlock();
        return true;
    });

    /* 必须等待协程返回才可以释放cond */
    lock.lock();
    m_run_status = COND_FREE;
    m_co_event_id = 0;
    return 0;
}

int CoCond::Notify()
{
    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "please use CoCond in coroutine!");
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "running a non-corourine!");      // 当前运行的非协程

    std::unique_lock<std::mutex> lock(m_co_event_mutex);

    if (m_co_event_id == 0 || m_run_status != COND_WAIT)
        return -1;

    m_run_status = COND_ACTIVE;
    if (g_bbt_poller->Notify(m_co_event_id, PollEventType::POLL_EVENT_CUSTOM, CoPollEventCustom::POLL_EVENT_CUSTOM_COND) != 0)
        return -1;

    return 0;
}

int CoCond::OnNotify(short trigger_event, int custom_key)
{
    auto wait_co = GetWaitCo();
    Assert(wait_co != nullptr);

    std::lock_guard<std::mutex> _(m_co_event_mutex);

    Assert(m_run_status == COND_ACTIVE || m_run_status == COND_WAIT);
    if (m_run_status != COND_ACTIVE || m_run_status != COND_WAIT)
    wait_co->Active();    
    // g_bbt_dbgp_full(("[CoEvent:Trigger] co=" + std::to_string(wait_co->GetId()) +
    //                                   " trigger_event=" + std::to_string(trigger_event) +
    //                                   " id=" + std::to_string(GetId()) +
    //                                   " customkey=" + std::to_string(custom_key)).c_str());

    return 0;
}


}