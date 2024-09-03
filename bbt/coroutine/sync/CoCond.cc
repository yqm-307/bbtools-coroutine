#include <bbt/coroutine/sync/CoCond.hpp>

namespace bbt::coroutine::sync
{

CoCond::CoCond()
{
}

CoCond::~CoCond()
{
}

int CoCond::Wait(std::unique_lock<std::mutex> lock)
{
}

int CoCond::WaitFor(std::unique_lock<std::mutex> lock, int ms)
{
}

void CoCond::NotifyAll()
{
    std::unique_lock<std::mutex> _(m_waiter_queue_mtx);
    while (_NotifyOne() == 0);
}

void CoCond::NotifyOne()
{
    std::unique_lock<std::mutex> _(m_waiter_queue_mtx);
    _NotifyOne();
}

int CoCond::_NotifyOne()
{
    if (m_waiter_queue.empty())
        return -1;
    
    auto waiter = m_waiter_queue.front();
    return waiter.Notify();
}

}