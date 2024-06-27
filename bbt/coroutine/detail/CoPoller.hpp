#pragma once
#include <sys/epoll.h>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/interface/IPoller.hpp>

namespace bbt::coroutine::detail
{

/* 协程轮询器，对于协程挂起条件是否完成进行轮询，达成条件时回调 */
class CoPoller:
    public IPoller
{
public:
    typedef std::unique_ptr<CoPoller> UPtr;
    static UPtr&                    GetInstance();

    CoPoller();
    ~CoPoller();

    virtual int                     AddFdEvent(std::shared_ptr<IPollEvent> event, int fd) override;
    virtual int                     DelFdEvent(int fd) override;
    virtual int                     ModifyFdEvent(std::shared_ptr<IPollEvent> event, int fd, int event_opt) override;
    virtual int                     PollOnce() override;

    int                             OnCustomEvent(std::shared_ptr<IPollEvent> event, int key);
protected:
private:
    int                             m_epoll_fd{-1};

    std::unordered_set<std::shared_ptr<IPollEvent>>
                                    m_safe_active_set;              // 保证不重复的事件
    std::queue<std::shared_ptr<IPollEvent>>
                                    m_custom_event_active_queue;    // 自定义事件活跃队列
    std::mutex                      m_custom_event_active_queue_mutex;
};

}