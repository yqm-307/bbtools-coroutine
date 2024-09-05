#include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/utils/DebugPrint.hpp>

namespace bbt::coroutine::sync
{

CoCond::SPtr CoCond::Create(std::mutex& lock)
{
    return std::make_shared<CoCond>(lock);
}

CoCond::CoCond(std::mutex& lock):
    m_lock_ref(lock)
{
}

CoCond::~CoCond()
{
    std::unique_lock<std::mutex> lock{m_lock_ref};
    if (!m_waiter_queue.empty())
        g_bbt_dbgp_full((std::to_string(m_waiter_queue.size()) + " coroutine maybe have been lost permanently!").c_str());
    // NotifyAll();
}

int CoCond::Wait()
{
    std::unique_lock<std::mutex> lock{m_lock_ref};
    auto waiter = CoWaiter::Create(true);
    m_waiter_queue.push(waiter);

    int ret = waiter->WaitWithCallback([&lock](){
        lock.unlock();
        return true;
    });

    return ret;
}

int CoCond::WaitFor(int ms)
{
    std::unique_lock<std::mutex> lock{m_lock_ref};
    auto waiter = CoWaiter::Create(true);
    m_waiter_queue.push(waiter);

    int ret = waiter->WaitWithTimeoutAndCallback(ms, [&lock](){
        lock.unlock();
        return true;
    });

    return ret;
}

void CoCond::NotifyAll()
{
    std::unique_lock<std::mutex> lock{m_lock_ref};
    while (!m_waiter_queue.empty())
        _NotifyOne();
}

void CoCond::NotifyOne()
{
    std::unique_lock<std::mutex> lock{m_lock_ref};
    _NotifyOne();
}

int CoCond::_NotifyOne()
{
    int ret = -1;
    if (m_waiter_queue.empty())
        return ret;
    
    while (!m_waiter_queue.empty()) {
        auto waiter = m_waiter_queue.front();
        m_waiter_queue.pop();
        if (waiter->Notify() == 0) {
            ret = 0;
            break;
        }
    }

    return ret;
}

}