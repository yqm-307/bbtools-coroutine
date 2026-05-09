#include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/utils/DebugPrint.hpp>

namespace bbt::coroutine::sync
{

CoCond::SPtr CoCond::Create()
{
    return std::make_shared<CoCond>(PrivateTag{});
}

CoCond::CoCond(PrivateTag)
{
}

CoCond::~CoCond()
{
    NotifyAll();
    {
        std::lock_guard<std::mutex> lock(m_waiter_guard);
        m_waiter_holders.clear();
    }
}

int CoCond::Wait()
{
    auto waiter = CoWaiter::Create();

    return waiter->WaitWithCallback([this, waiter](){
        {
            std::lock_guard<std::mutex> lock(m_waiter_guard);
            m_waiter_holders[waiter.get()] = waiter;
        }
        m_waiter_queue.Push(waiter.get());
        return true;
    });
}

int CoCond::WaitFor(int ms)
{
    auto waiter = CoWaiter::Create();
    
    return waiter->WaitWithTimeoutAndCallback(ms, [this, waiter](){
        {
            std::lock_guard<std::mutex> lock(m_waiter_guard);
            m_waiter_holders[waiter.get()] = waiter;
        }
        m_waiter_queue.Push(waiter.get());
        return true;
    });
}

void CoCond::NotifyAll()
{
    while (!m_waiter_queue.Empty())
        _NotifyOne();
}

int CoCond::NotifyOne()
{
    return _NotifyOne();
}

int CoCond::_NotifyOne()
{
    int ret = -1;
    while (!m_waiter_queue.Empty()) {
        CoWaiter* waiter = nullptr;
        if (m_waiter_queue.Pop(waiter))
        {
            CoWaiter::SPtr holder = nullptr;
            {
                std::lock_guard<std::mutex> lock(m_waiter_guard);
                auto iter = m_waiter_holders.find(waiter);
                if (iter == m_waiter_holders.end())
                    continue;

                holder = iter->second;
                m_waiter_holders.erase(iter);
            }

            ret = holder->Notify();
            if (ret == 0)
                break;
        }
    }

    return ret;
}

}