#include <unistd.h>
#include <fcntl.h>
#include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>

namespace bbt::coroutine::sync
{

using namespace bbt::coroutine::detail;

CoCond::SPtr CoCond::Create()
{
    auto cond = std::make_shared<CoCond>();
    if (cond->Init() != 0)
        return nullptr;

    return cond;
}


CoCond::CoCond()
{
}

CoCond::~CoCond()
{
}

int CoCond::Init()
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return -1;

    return 0;
}

int CoCond::Wait()
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return -1;
    
    auto current_co = g_bbt_tls_coroutine_co;
    if (current_co == nullptr)
        return -1;
    {
        std::unique_lock<std::mutex> _(m_co_event_mutex);
        if (m_co_event != nullptr)
            return -1;
        
        m_co_event = current_co->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND);
        if (m_co_event == nullptr)
            return -1;
    }

    current_co->Yield();
    
    std::unique_lock<std::mutex> _(m_co_event_mutex);
    m_co_event = nullptr;
    return 0;
}

int CoCond::WaitWithTimeout(int ms)
{
    if (!g_bbt_tls_helper->EnableUseCo())
        return -1;
    
    auto current_co = g_bbt_tls_coroutine_co;
    if (current_co == nullptr)
        return -1;
    {
        std::unique_lock<std::mutex> _(m_co_event_mutex);
        if (m_co_event != nullptr)
            return -1;
        
        m_co_event = current_co->RegistCustom(detail::CoPollEventCustom::POLL_EVENT_CUSTOM_COND, ms);
        if (m_co_event == nullptr)
            return -1;
    }

    current_co->Yield();
    
    std::unique_lock<std::mutex> _(m_co_event_mutex);
    m_co_event = nullptr;
    return 0;
}

int CoCond::Notify()
{
    std::unique_lock<std::mutex> _(m_co_event_mutex);
    if (!g_bbt_tls_helper->EnableUseCo())
        return -1;

    if (m_co_event == nullptr)
        return -1;
    
    if (g_bbt_poller->NotifyCustomEvent(m_co_event) != 0)
        return -1;

    return 0;
}

}