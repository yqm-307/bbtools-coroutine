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
    _SysLock();
    if (!m_waiter_queue.empty())
        g_bbt_dbgp_full((std::to_string(m_waiter_queue.size()) + " coroutine maybe have been lost permanently!").c_str());
    _SysUnLock();
}

int CoCond::Wait()
{
    auto waiter = CoWaiter::Create();

    return waiter->WaitWithCallback([waiter, this](){
        _SysLock();
        m_waiter_queue.push(waiter);
        _SysUnLock();
        return true;
    });
}

int CoCond::WaitFor(int ms)
{
    auto waiter = CoWaiter::Create();

    return waiter->WaitWithTimeoutAndCallback(ms, [waiter, this](){
        _SysLock();
        m_waiter_queue.push(waiter);
        _SysUnLock();
        return true;
    });
}

void CoCond::NotifyAll()
{
    while (true) {
        _SysLock();
        if (m_waiter_queue.empty()) {
            _SysUnLock();
            break;
        }
        auto waiter = m_waiter_queue.front();
        m_waiter_queue.pop();
        _SysUnLock();
        waiter->Notify();
    }
}

int CoCond::NotifyOne()
{
    return _NotifyOne();
}

int CoCond::_NotifyOne()
{
    int ret = -1;
    _SysLock();
    while (!m_waiter_queue.empty()) {
        auto waiter = m_waiter_queue.front();
        m_waiter_queue.pop();
        ret = waiter->Notify();
        if (ret == 0)
            break; // 成功唤醒
        // ret == -1: 已取消或已触发，继续尝试下一个
    }
    _SysUnLock();
    return ret;
}

} // namespace bbt::coroutine::sync
