#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Processer.hpp>

namespace bbt::coroutine::detail
{

template<class TPollEvent>
std::pair<int, CoPollEventId> CoPoller::Regist(std::shared_ptr<IPollEvent> event, int ms)
{
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "non-coroutine!");
    static_assert(std::is_base_of_v<CoEventBase, TPollEvent>);

    if (ms <= 0) return {-1, 0};

    std::lock_guard<std::mutex> _(m_event_map_mtx);
    return _RegistEvent<TPollEvent>(event, -1, ms, pollevent::EventOpt::TIMEOUT, POLL_EVENT_CUSTOM_DEFAULT);
}

template<class TPollEvent>
std::pair<int, CoPollEventId> CoPoller::RegistCustom(std::shared_ptr<IPollEvent> event, CoPollEventCustom custom_key, int ms)
{
    static_assert(std::is_base_of_v<CoEventBase, TPollEvent>);

    std::lock_guard<std::mutex> _(m_event_map_mtx);
    return _RegistEvent<TPollEvent>(event, -1, ms, 0, custom_key);
}

template<class TPollEvent>
std::pair<int, CoPollEventId> CoPoller::RegistFdReadable(std::shared_ptr<IPollEvent> event, int fd, int ms)
{
    static_assert(std::is_base_of_v<CoEventBase, TPollEvent>);
    if (fd < 0) return {-1, 0};

    std::lock_guard<std::mutex> _(m_event_map_mtx);
    return _RegistEvent<TPollEvent>(event, fd, ms, pollevent::EventOpt::READABLE | pollevent::EventOpt::FINALIZE, POLL_EVENT_CUSTOM_DEFAULT);
}

template<class TPollEvent>
std::pair<int, CoPollEventId> CoPoller::RegistFdWriteable(std::shared_ptr<IPollEvent> event, int fd, int ms)
{
    static_assert(std::is_base_of_v<CoEventBase, TPollEvent>);
    if (fd < 0) return {-1, 0};
    
    std::lock_guard<std::mutex> _(m_event_map_mtx);
    return _RegistEvent<TPollEvent>(event, fd, ms, pollevent::EventOpt::WRITEABLE | pollevent::EventOpt::FINALIZE, POLL_EVENT_CUSTOM_DEFAULT);
}

template<class TPollEvent>
std::pair<int, CoPollEventId> CoPoller::_RegistEvent(std::shared_ptr<IPollEvent> event, int fd, int ms, short events, CoPollEventCustom custom_key)
{
    Assert(event != nullptr);
    bool need_fd_event = ms > 0 || fd >= 0;

    if (need_fd_event && (event->InitFdEvent(fd, events, ms) < 0))
        return {-1, 0};

    if (custom_key != CoPollEventCustom::POLL_EVENT_CUSTOM_DEFAULT)
        event->InitCustomEvent(custom_key);

    if (event->Regist() != 0)
        return {-1, 0};

    return Regist(event);
}

}