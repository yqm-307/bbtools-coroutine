#include <bbt/base/assert/Assert.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

namespace bbt::coroutine::sync
{

std::shared_ptr<CoMutex> CoMutex::Create()
{
    return std::make_shared<CoMutex>();
}

CoMutex::CoMutex()
{

}

CoMutex::~CoMutex()
{

}

void CoMutex::Lock()
{
    std::unique_lock<std::mutex> lock(m_mutex);

    while (m_status == CoMutexStatus::COMUTEX_LOCKED) {
        Assert(_RegistEvent() == 0);

        GetWaitCo()->YieldWithCallback([&lock](){
            lock.unlock();
            return true;
        });

        lock.lock();
    }

    Assert(m_status == CoMutexStatus::COMUTEX_FREE);
    m_status = CoMutexStatus::COMUTEX_LOCKED;
}

void CoMutex::UnLock()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_status = CoMutexStatus::COMUTEX_FREE;
    _NotifyOne();
}

int CoMutex::TryLock()
{

    int ret = 0;
    std::lock_guard<std::mutex> _(m_mutex);

    if (m_status == CoMutexStatus::COMUTEX_FREE)
        m_status = CoMutexStatus::COMUTEX_LOCKED;
    else
        ret = -1;

    return ret;
}

int CoMutex::_RegistEvent()
{        
    auto [ret, id] = g_bbt_poller->RegistCustom<CoMutex>(shared_from_this(), detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COMUTEX);
    if (ret != 0)
        return -1;

    m_wait_event_queue.push(std::make_pair(id, g_bbt_tls_coroutine_co));
    return 0;
}

void CoMutex::_NotifyOne()
{
    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "please use CoCond in coroutine!");

    std::lock_guard<std::mutex> _(m_mutex);
    if (m_wait_event_queue.empty())
        return;

    auto event = m_wait_event_queue.front();
    int ret = g_bbt_poller->Notify(event.first, detail::PollEventType::POLL_EVENT_CUSTOM, detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COMUTEX);
    Assert(ret == 0);
}

int CoMutex::OnNotify(short trigger_events, int customkey)
{
    if (m_wait_event_queue.empty())
        return -1;
    
    auto event = m_wait_event_queue.front();
    m_wait_event_queue.pop();

    event.second->Active();
    return 0;
}

}