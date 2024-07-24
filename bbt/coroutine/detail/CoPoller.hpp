#pragma once
#include <sys/epoll.h>
#include <bbt/pollevent/EventLoop.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/interface/IPoller.hpp>

namespace bbt::coroutine::detail
{

/* 协程轮询器，对于协程挂起条件是否完成进行轮询，达成条件时回调 */
/**
 * @brief 事件轮询器
 * 
 * 为了让协程因为阻塞而挂起，因为条件达成而唤醒
 * 
 * 目前设计分为多个模块：
 *  1、自定义事件，由用户主动调用来让事件被唤醒；
 *  2、系统fd事件，由操作系统通知让事件被唤醒（使用epoll监视事件），
 *     目前只有timerfd，后续添加更多对系统操作的hook
 * 
 */
class CoPoller
{
public:
    typedef std::unique_ptr<CoPoller> UPtr;
    static UPtr&                    GetInstance();

    CoPoller();
    ~CoPoller();

    /* 是否有活跃事件 */
    bool                            PollOnce();

    std::shared_ptr<bbt::pollevent::Event> 
                                    CreateEvent(int fd, short events, const bbt::pollevent::OnEventCallback& onevent_cb);

    void                            NotifyCustomEvent(std::shared_ptr<CoPollEvent> event);
    /* 获取CoPoller缓存的UTC时间戳 */
    int64_t                         GetTime();
protected:
private:
    std::shared_ptr<bbt::pollevent::EventLoop> m_event_loop{nullptr};
    // int                             m_epoll_fd{-1};

    std::unordered_set<std::shared_ptr<CoPollEvent>>
                                    m_safe_active_set;              // 保证不重复的事件
    std::queue<std::shared_ptr<CoPollEvent>>
                                    m_custom_event_active_queue;    // 自定义事件活跃队列
    std::mutex                      m_custom_event_active_queue_mutex;
};

}