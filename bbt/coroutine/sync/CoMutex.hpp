#pragma once
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>
#include <bbt/coroutine/sync/interface/ICoLock.hpp>

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
    public ICoLock
{
public:
    typedef std::shared_ptr<CoMutex> SPtr;
    static SPtr Create();

    BBTATTR_FUNC_Ctor_Hidden
    CoMutex();
    ~CoMutex();

    void                        Lock();
    void                        UnLock();

    /**
     * @brief 获取锁，如果无法立即获取挂起协程直到超时
     * @param ms 
     * @return int 0成功，-1发生错误，1表示超时
     */
    int                         TryLock(int ms);

    /**
     * @brief 尝试获取锁，立即返回
     * @return int 0成功，-1表示失败
     */
    int                         TryLock();

protected:
    int                         _WaitUnLockUnitlTimeout(int timeout, const detail::CoroutineOnYieldCallback& cb);
    int                         _WaitUnLock(const detail::CoroutineOnYieldCallback& cb);

    void                        _NotifyOne();

    void                        _SysLock();
    void                        _SysUnLock();
private:
    std::queue<std::shared_ptr<detail::CoPollEvent>> 
                                m_wait_event_queue;
    // std::queue<CoCond::SPtr>    m_wait_lock_queue;
    std::mutex                  m_mutex;
    volatile CoMutexStatus      m_status{CoMutexStatus::COMUTEX_FREE};
};

} // bbt::coroutine::sync