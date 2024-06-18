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
    static UPtr& GetInstance();

    CoPoller();
    ~CoPoller();

    virtual int AddEvent(std::shared_ptr<IPollEvent> event, int addevent) override;
    virtual int DelEvent(std::shared_ptr<IPollEvent> event, int delevent) override;
    virtual int ModifyEvent(std::shared_ptr<IPollEvent> event, int modify_event) override;
    virtual void PollOnce() override;

    /* 这个类用来包裹对象的智能指针，防止事件被意外被释放 */
    struct PrivData {std::shared_ptr<IPollEvent> event_sptr{nullptr};};
protected:
private:
    int                             m_epoll_fd;
};

}