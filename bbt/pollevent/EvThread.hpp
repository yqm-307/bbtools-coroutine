#pragma once
#include <thread>
#include <memory>

#include <bbt/pollevent/detail/Define.hpp>

namespace bbt::pollevent
{

/* io线程id，小于0是非法值 */

class EvThread final:
    public std::enable_shared_from_this<EvThread>
{
public:
    typedef std::shared_ptr<EvThread> SPtr;

    explicit EvThread(std::shared_ptr<EventLoop> evloop);
    EvThread();
    virtual ~EvThread();

    /**
     * @brief 启动当前线程（非线程安全）
     */
    void                    Start();

    /**
     * @brief 停止当前线程（非线程安全）
     */
    core::errcode::ErrOpt   Stop();

    EvThreadId              GetTid() const;
    bool                    IsRunning() const;

    /**
     * @brief 在当前线程中注册一个事件，并且在当前线程中触发（线程安全）
     * 
     * @param fd 
     * @param events 
     * @param onevent_cb 
     * @return std::shared_ptr<Event> 
     */
    std::shared_ptr<Event>  RegisterEvent(evutil_socket_t fd, short events, const bbt::pollevent::OnEventCallback& onevent_cb);
    void                    Join();
    std::shared_ptr<EventLoop> GetEventLoop() const;
protected:
    virtual void            WorkHandle();
    std::thread::id         GetSysTid() const;
    void                    StartWorkFunc();
    void                    SyncWaitThreadExit();
    bool                    SyncWaitThreadExitWithTime(int wait_time);
private:
    void                    Destory();
    void                    Work();
    bool                    SyncWaitThreadExitEx(int wait_time);
    static EvThreadId       GenerateTid();
private:
    std::shared_ptr<bbt::pollevent::EventLoop> m_eventloop{nullptr};
    std::thread*        m_thread{nullptr};
    EvThreadId          m_tid{-1};
    volatile EvThreadRunStatus   m_status{EvThreadRunStatus::Default};
};



} // namespace bbt::pollevent
