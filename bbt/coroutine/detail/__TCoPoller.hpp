#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>

namespace bbt::coroutine::detail
{

template<class TPollEvent>
std::pair<int, CoPollEventId> CoPoller::Regist(int ms)
{
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "non-coroutine!");
    if (ms <= 0) return {-1, 0};

    std::lock_guard<std::mutex> _(m_event_map_mtx);
    return _RegistEvent<TPollEvent>(-1, ms, pollevent::EventOpt::TIMEOUT, POLL_EVENT_CUSTOM_DEFAULT);
}

template<class TPollEvent>
std::pair<int, CoPollEventId> CoPoller::RegistCustom(CoPollEventCustom custom_key, int ms)
{
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "non-coroutine!");

    std::lock_guard<std::mutex> _(m_event_map_mtx);
    return _RegistEvent<TPollEvent>(-1, ms, 0, custom_key);
}

template<class TPollEvent>
std::pair<int, CoPollEventId> CoPoller::RegistFdReadable(int fd, int ms)
{
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "non-coroutine!");
    if (fd < 0) return {-1, 0};

    std::lock_guard<std::mutex> _(m_event_map_mtx);
    return _RegistEvent<TPollEvent>(fd, ms, pollevent::EventOpt::READABLE | pollevent::EventOpt::FINALIZE, POLL_EVENT_CUSTOM_DEFAULT);
}

template<class TPollEvent>
std::pair<int, CoPollEventId> CoPoller::RegistFdWriteable(int fd, int ms)
{
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "non-coroutine!");
    if (fd < 0) return {-1, 0};
    
    std::lock_guard<std::mutex> _(m_event_map_mtx);
    return _RegistEvent<TPollEvent>(fd, ms, pollevent::EventOpt::WRITEABLE | pollevent::EventOpt::FINALIZE, POLL_EVENT_CUSTOM_DEFAULT);
}

template<class TPollEvent>
std::pair<int, CoPollEventId> CoPoller::_RegistEvent(int fd, int ms, short events, CoPollEventCustom custom_key)
{
    bool need_fd_event = ms > 0 || fd >= 0;

    auto event = TPollEvent::Create();

    AssertWithInfo(event->BindCoroutine(g_bbt_tls_coroutine_co) == 0, "");

    if (need_fd_event && (event->InitFdEvent(fd, events, ms) < 0))
        return {-1, 0};

    if (custom_key != CoPollEventCustom::POLL_EVENT_CUSTOM_DEFAULT)
        event->InitCustomEvent(custom_key);

    if (event->Regist() != 0)
        return {-1, 0};

    return Regist(event);
}

}