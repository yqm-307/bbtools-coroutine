#include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/detail/DriverWakeDeferral.hpp>
#include <bbt/coroutine/utils/DebugPrint.hpp>

namespace bbt::coroutine::sync {

CoCond::SPtr CoCond::Create()
{
    return std::make_shared<CoCond>(PrivateTag{});
}

CoCond::CoCond(PrivateTag) {}

CoCond::~CoCond()
{
    _SysLock();
    if (!m_waiter_queue.empty())
        g_bbt_dbgp_full(
            (std::to_string(m_waiter_queue.size()) + " coroutine maybe have been lost permanently!").c_str());
    _SysUnLock();
}

int CoCond::Wait()
{
    auto waiter = CoWaiter::Create();

    /**
     * 入队放在挂起回调里：回调只在挂起成功后执行，Notify 不会 pop 到未登记的 waiter（那会返回 -1 丢唤醒）。
     */
    return waiter->WaitWithCallback([waiter, this]() {
        _SysLock();
        m_waiter_queue.push(waiter);
        _SysUnLock();
        return true;
    });
}

int CoCond::WaitFor(int ms)
{
    auto waiter = CoWaiter::Create();

    /**
     * 同 Wait：入队放在挂起回调里，避免 Notify 命中尚未登记的 waiter。
     */
    return waiter->WaitWithTimeoutAndCallback(ms, [waiter, this]() {
        _SysLock();
        m_waiter_queue.push(waiter);
        _SysUnLock();
        return true;
    });
}

void CoCond::NotifyAll()
{

    /**
     * 逐个出队后在锁外 Notify：唤醒链不持 m_mutex，期间新到的等待者仍能入队。
     */
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

    /**
     * waiter->Notify() 的触发链在 m_mutex 内执行；driver Wake 推迟到本函数退出之后。
     */
    detail::DriverWakeDeferral _defer;
    _SysLock();
    while (!m_waiter_queue.empty()) {
        auto waiter = m_waiter_queue.front();
        m_waiter_queue.pop();
        ret = waiter->Notify();

        /**
         * 成功唤醒
         */
        if (ret == 0)
            break;

        /**
         * ret == -1: 已取消或已触发，继续尝试下一个
         */
    }
    _SysUnLock();
    return ret;
}

}
