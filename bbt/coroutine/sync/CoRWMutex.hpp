#pragma once
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
#include <unordered_map>

namespace bbt::coroutine::sync
{

class CoRWMutex
{
    struct PrivateTag {};
    enum class FairnessPolicy
    {
        WriterPreferred,
    };
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
    static constexpr FairnessPolicy kFairnessPolicy = FairnessPolicy::WriterPreferred;

    int _NotifyOne();
    int _NotifyAll(bool reader);
    int _WaitRLock(detail::CoroutineOnYieldCallback&& cb);
    int _WaitWLock(detail::CoroutineOnYieldCallback&& cb);
    bool _HasWaitingWriterLocked() const;
    bool _ShouldBlockReaderLocked() const;

    void _SysLock();
    void _SysUnLock();
private:
    std::queue<std::shared_ptr<CoWaiter>>
                                m_wait_readlock_queue;
    std::queue<std::shared_ptr<CoWaiter>>
                                m_wait_writelock_queue;

    int                         m_rlock_hold_num{0};
    detail::Coroutine::Ptr      m_wlock_owner{nullptr};
    std::unordered_map<detail::Coroutine::Ptr, int> m_rlock_owners;

    std::mutex                  m_mutex;
    CoRWMutexStatus             m_status{CORWMUTEX_FREE};
};

} // bbt::coroutine::sync
