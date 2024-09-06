#include <bbt/pollevent/Event.hpp>
#include <bbt/coroutine/syntax/_EventHelper.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

namespace bbt::coroutine
{

_EventHelper::_EventHelper(int fd, short event, int ms):
    m_fd(fd), m_event(event), m_timeout(ms)
{
    if (m_event > 0)
        m_arg_check_rlt = true;
}

_EventHelper::~_EventHelper()
{

}


int _EventHelper::operator-(const std::function<void()>& event_handle)
{
    if (!m_arg_check_rlt)
        return -1;

    /* 延长生命期到事件触发 */
    std::shared_ptr<_EventHelper> pthis = shared_from_this();

    /* 当事件触发时，注册一个事件处理协程 */
    m_pollevent = g_bbt_poller->CreateEvent(m_fd, m_event, [pthis, event_handle](auto event_ptr, short event){
        g_scheduler->RegistCoroutineTask(std::forward<const std::function<void()>&>(event_handle));

        /* 防止循环引用 */
        event_ptr = nullptr;
        pthis->m_pollevent = nullptr;
    });

    return m_pollevent->StartListen(m_timeout);
}

}