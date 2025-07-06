#include <bbt/pollevent/Event.hpp>
#include <bbt/coroutine/syntax/_EventHelper.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/pool/CoPool.hpp>

namespace bbt::coroutine
{

_EventHelper::_EventHelper(int fd, short event, int ms):
    m_fd(fd), m_timeout(ms), m_event(event)
{
    Assert((m_event & bbt::pollevent::PERSIST) == 0);
    if (m_event > 0)
        m_arg_check_rlt = true;
}

_EventHelper::_EventHelper(int fd, short event, int ms, std::shared_ptr<pool::CoPool> pool):
    m_copool(pool), m_fd(fd), m_timeout(ms), m_event(event)
{
    Assert((m_event & bbt::pollevent::PERSIST) == 0);
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
    m_pollevent = g_bbt_poller->CreateEvent(m_fd, m_event, [pthis, event_handle](int fd, short type, bbt::pollevent::EventId eventid){
        /**
         * 这里有个隐蔽的循环引用问题：
         * 
         * _EventHelper 引用 Event；function 持有 _EventHelper；Event 持有 function
         * Event 变相持有 _EventHelper
         * 
         * 所以这里在触发事件后，将_EventHelper中持有的Event释放掉，打破循环引用
         */
        auto on_event_handle = [=](){ 
            /**
             * 如果是持久事件且使用者不想释放，直接返回。这样m_pollevent不会释放，下次触发再
             * 根据使用者提供的返回值来判断是否释放事件
             */
            event_handle(fd, type);
            pthis->m_pollevent = nullptr;
        };

        /* 支持使用协程池处理 */
        auto copool = pthis->m_copool.lock();
        if (copool)
            copool->Submit(std::move(on_event_handle));
        else
            g_scheduler->RegistCoroutineTask(std::move(on_event_handle));
    });

    return m_pollevent->StartListen(m_timeout);
}

}