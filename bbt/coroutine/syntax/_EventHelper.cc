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


int _EventHelper::operator-(const ExtCoEventCallback& event_handle)
{
    if (!m_arg_check_rlt)
        return -1;

    /* 延长生命期到事件触发，让event function持有智能指针，当function执行完毕会释放 */
    std::shared_ptr<_EventHelper> pthis = shared_from_this();

    /* 当事件触发时，注册一个事件处理协程 */
    m_pollevent = g_bbt_poller->CreateEvent(m_fd, m_event, [pthis, event_handle](auto ev, short type){
        g_scheduler->RegistCoroutineTask([=](){ event_handle(ev->GetSocket(), type); });

        /**
         * 这里有个隐蔽的循环引用问题：
         * 
         * _EventHelper 引用 Event；function 持有 _EventHelper；Event 持有 function
         * Event 变相持有 _EventHelper
         * 
         * 所以这里在触发事件后，将_EventHelper中持有的Event释放掉，打破循环引用
         */
        pthis->m_pollevent = nullptr;
    });

    return m_pollevent->StartListen(m_timeout);
}

}