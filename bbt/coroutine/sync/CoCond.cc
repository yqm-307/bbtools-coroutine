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
}

int CoCond::Wait()
{
    auto waiter = CoWaiter::Create();

    return waiter->WaitWithCallback([this, waiter](){
        std::lock_guard<std::mutex> lock(m_waiter_guard);
        m_waiter_queue.push(waiter);
        return true;
    });
}

int CoCond::WaitFor(int ms)
{
    auto waiter = CoWaiter::Create();
    
    return waiter->WaitWithTimeoutAndCallback(ms, [this, waiter](){
        std::lock_guard<std::mutex> lock(m_waiter_guard);
        m_waiter_queue.push(waiter);
        return true;
    });
}

void CoCond::NotifyAll()
{
    while (_NotifyOne() == 0) {
    }
}

int CoCond::NotifyOne()
{
    return _NotifyOne();
}

int CoCond::_NotifyOne()
{
    int ret = -1;
    while (true) {
        CoWaiter::SPtr waiter = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_waiter_guard);
            if (m_waiter_queue.empty())
                break;

            waiter = m_waiter_queue.front();
            m_waiter_queue.pop();
        }

        ret = waiter->Notify();
        if (ret == 0)
            break;
    }

    return ret;
}

}
