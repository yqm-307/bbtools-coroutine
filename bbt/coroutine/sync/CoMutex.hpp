#pragma once
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>

namespace bbt::coroutine::sync
{

/**
 * 用来实现协程间的同步
 * 
 * 当多个协程有数据竞争时，使用Mutex可以让发生竞争的锁
 * 挂起等待，当有获取锁的协程解锁后，将唤醒一个竞争中的
 * 协程。
 * 
 * 使用此锁是为了防止阻塞系统线程
 * 
 * XXX
 * 目前实现为使用CoCond来实现。CoCond内部需要加锁导致部
 * 分锁是重叠的，最好的做法是像CoCond一样控制CoPollEvent
 */
class CoMutex:
    public bbt::coroutine::detail::CoEventBase,
    public std::enable_shared_from_this<CoMutex>
{
public:
    static std::shared_ptr<CoMutex> Create();
    CoMutex();
    ~CoMutex();

    void                        Lock();
    void                        UnLock();
    int                         TryLock();

protected:
    virtual int                 OnNotify(short trigger_events, int customkey) override;
    int                         _RegistEvent();
    void                        _NotifyOne();

private:
    std::queue<std::pair<detail::CoPollEventId, std::shared_ptr<detail::Coroutine>>>
                                m_wait_event_queue;
    std::mutex                  m_mutex;
    volatile CoMutexStatus      m_status{CoMutexStatus::COMUTEX_FREE};
};

} // bbt::coroutine::sync