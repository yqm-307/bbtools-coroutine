#pragma once
#include <bbt/coroutine/sync/CoWaiter.hpp>

namespace bbt::coroutine::sync
{

class CoCond
{
public:
    typedef std::shared_ptr<CoCond> SPtr;

    static SPtr Create(std::mutex& lock);

    BBTATTR_FUNC_Ctor_Hidden
    CoCond(std::mutex& lock);
    virtual ~CoCond();

    int                         Wait();
    int                         WaitFor(int ms);

    int                         NotifyOne();
    void                        NotifyAll();
protected:
    int                         _NotifyOne();
private:
    std::queue<std::shared_ptr<CoWaiter>>       m_waiter_queue;
    std::mutex&                                 m_lock_ref;
};

} // namespace bbt::coroutine::sync