#pragma once
#include <bbt/coroutine/sync/CoWaiter.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>

namespace bbt::coroutine::sync
{

class CoRWMutex
{
    struct PrivateTag {};
public:
    typedef std::shared_ptr<CoRWMutex> SPtr;
    static SPtr Create();
    BBTATTR_FUNC_CTOR_HIDDEN 
    CoRWMutex(PrivateTag) {}
    ~CoRWMutex() {}

    int RLock();
    int WLock();
    int UnLock();

protected:
    int _NotifyOne();
    int _NotifyAll(bool reader);
    int _WaitRLock(detail::CoroutineOnYieldCallback&& cb);
    int _WaitWLock(detail::CoroutineOnYieldCallback&& cb);

    void _SysLock();
    void _SysUnLock();
private:
    std::queue<std::shared_ptr<CoWaiter>>
                                m_wait_readlock_queue;
    std::queue<std::shared_ptr<CoWaiter>>
                                m_wait_writelock_queue;

    int                         m_rlock_hold_num{0};
    bool                        m_has_wait_wlock{false};

    std::mutex                  m_mutex;
    CoRWMutexStatus             m_status{CORWMUTEX_FREE};
};

} // bbt::coroutine::sync