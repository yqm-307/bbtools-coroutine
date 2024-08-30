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
class CoPoller:
    public IPoller
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

    template<class TPollEvent>
    std::pair<int, CoPollEventId>   Regist(int ms);
    template<class TPollEvent>
    std::pair<int, CoPollEventId>   RegistCustom(CoPollEventCustom custom_key, int ms = -1);
    template<class TPollEvent>
    std::pair<int, CoPollEventId>   RegistFdReadable(int fd, int ms = -1);
    template<class TPollEvent>
    std::pair<int, CoPollEventId>   RegistFdWriteable(int fd, int ms = -1);

    int                             UnRegist(CoPollEventId event) override;
    int                             Notify(CoPollEventId eventid, short trigger_event, CoPollEventCustom custom_key) override;
protected:
    template<class TPollEvent>
    std::pair<int, CoPollEventId>   _RegistEvent(int fd, int ms, short events, CoPollEventCustom custom_key);
    virtual std::pair<int, CoPollEventId>   Regist(std::shared_ptr<IPollEvent> event) override;
private:
    std::shared_ptr<bbt::pollevent::EventLoop> m_event_loop{nullptr};

    std::unordered_map<CoPollEventId, std::shared_ptr<IPollEvent>>
                                    m_event_map;
    std::mutex                      m_event_map_mtx;

    std::queue<std::shared_ptr<CoPollEvent>>
                                    m_custom_event_active_queue;    // 自定义事件活跃队列
    std::mutex                      m_custom_event_active_queue_mutex;
};

}

#include <bbt/coroutine/detail/__TCoPoller.hpp>