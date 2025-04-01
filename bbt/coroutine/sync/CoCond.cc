#include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/utils/DebugPrint.hpp>

namespace bbt::coroutine::sync
{

CoCond::SPtr CoCond::Create()
{
    return std::make_shared<CoCond>();
}

CoCond::CoCond()
{
}

CoCond::~CoCond()
{
    if (!m_waiter_queue.Empty())
        g_bbt_dbgp_full((std::to_string(m_waiter_queue.Size()) + " coroutine maybe have been lost permanently!").c_str());
}

int CoCond::Wait()
{
    auto waiter = CoWaiter{};

    return waiter.WaitWithCallback([&](){
        m_waiter_queue.Push(&waiter);
        return true;
    });;
}

int CoCond::WaitFor(int ms)
{
    auto waiter = CoWaiter{};
    
    return waiter.WaitWithTimeoutAndCallback(ms, [&](){
        m_waiter_queue.Push(&waiter);
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
            ret = waiter->Notify();
            break;
        }
    }

    return ret;
}

}