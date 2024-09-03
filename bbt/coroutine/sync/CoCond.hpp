#pragma once
#include <bbt/coroutine/sync/CoWaiter.hpp>

namespace bbt::coroutine::sync
{

class CoCond
{
public:
    CoCond();
    virtual ~CoCond();

    int                         Wait(std::unique_lock<std::mutex> lock);

    int                         WaitFor(std::unique_lock<std::mutex> lock, int ms);

    void                        NotifyOne();
    void                        NotifyAll();
protected:
    int                         _NotifyOne();
private:
    std::queue<CoWaiter>        m_waiter_queue;
    std::mutex                  m_waiter_queue_mtx;
};

} // namespace bbt::coroutine::sync