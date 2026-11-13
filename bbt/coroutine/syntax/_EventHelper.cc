#include <bbt/pollevent/Event.hpp>
#include <bbt/coroutine/syntax/_EventHelper.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/pool/CoPool.hpp>

namespace bbt::coroutine {

_EventHelper::_EventHelper(int fd, short event, int ms) : m_fd(fd), m_timeout(ms), m_event(event)
{

    /**
     * 一次性事件：触发即释放。PERSIST 会在触发后重挂，和「回调里断开 Event」的生命期模型冲突
     */
    Assert((m_event & bbt::pollevent::PERSIST) == 0);
    if (m_event > 0)
        m_arg_check_rlt = true;
}

_EventHelper::_EventHelper(int fd, short event, int ms, std::shared_ptr<pool::CoPool> pool)
    : m_copool(pool), m_fd(fd), m_timeout(ms), m_event(event)
{
    Assert((m_event & bbt::pollevent::PERSIST) == 0);
    if (m_event > 0)
        m_arg_check_rlt = true;
}

_EventHelper::~_EventHelper() {}

/**
 * 参数检查失败返回 -1；否则原样返回 StartListen 的结果（0 已登记 / -1 失败，失败时回调不会被调用）。
 */
int _EventHelper::operator-(const ExtCoEventCallback& event_handle)
{
    if (!m_arg_check_rlt)
        return -1;

    /**
     * 延长生命期到事件触发，让event function持有智能指针，当function执行完毕会释放
     */
    std::shared_ptr<_EventHelper> pthis = shared_from_this();

    /**
     * 当事件触发时，注册一个事件处理协程
     */
    m_pollevent = g_bbt_poller->CreateEvent(m_fd, m_event,
                                            [pthis, event_handle](int fd, short type, bbt::pollevent::EventId eventid) {
                                                /**
                                                 * 循环引用：helper 持有 Event，Event 持有这个回调，回调又持有 helper。
                                                 * 所以回调执行完要把 helper 里的 Event 置空，否则双方都等不到析构。
                                                 */
                                                auto on_event_handle = [=]() {
                                                    /**
                                                     * 跑完即断开对 Event 的持有，一次性事件到此为止
                                                     */
                                                    event_handle(fd, type);
                                                    pthis->m_pollevent = nullptr;
                                                };

                                                /**
                                                 * 支持使用协程池处理；池已销毁则回退到直接注册协程
                                                 */
                                                auto copool = pthis->m_copool.lock();
                                                if (copool)
                                                    copool->Submit(std::move(on_event_handle));
                                                else
                                                    g_scheduler->RegistCoroutineTask(std::move(on_event_handle));
                                            });

    return m_pollevent->StartListen(m_timeout);
}

}
