#include <stdexcept>
#include <thread>
#include <bbt/coroutine/sync/CompletionSignal.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>
#include <bbt/coroutine/object/CoObject.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

namespace bbt::coroutine
{

CompletionSignal::CompletionSignal():
    m_waiter(sync::CoWaiter::Create()),
    m_generation(CurrentRuntimeGeneration())
{
    /* 契约 §C1：无运行时代际（未启动或已开始 Stop）不允许新建信号 */
    if (m_generation == 0)
        throw std::logic_error{"CompletionSignal: runtime generation unavailable"};
}

bool CompletionSignal::Complete() noexcept
{
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (m_completed)
            return false;
        m_completed = true;
    }

    /* 代际不符（Stop 中/已换代）只记录完成：等待者已被 Stop 回收，
     * 不得再访问（契约 §C1：Complete 可晚于 Stop）。
     * 同代际内在途 Stop 与 Trigger 的竞争由 CoPollEvent 状态机裁决：
     * UnRegist 先胜则本次触发是终态 no-op。 */
    if (detail::Scheduler::GetInstance()->GetRunGeneration() != m_generation)
        return true;

    /* 唤醒投递经 CoWaiter::Notify 完成——等待事件在窄接口内部创建与
     * 登记，Complete 不持事件句柄。m_completed 置位后新 Wait 在入口
     * 被拒，m_waiting 只减不增：在途等待者要么已建好事件（PARKED 或
     * 更早阶段触发被 PENDING 吸收，Notify 一次即达），要么尚在登记
     * 前的窗口内——此时 Notify 落到空等待位返回失败，重试至事件建好
     * 或等待者自行收场（取消/超时/登记失败）使计数归零。 */
    try {
        while (m_waiter->Notify() != 0) {
            {
                std::lock_guard<std::mutex> lock(m_mtx);
                if (m_waiting == 0)
                    return true;
            }
            std::this_thread::yield();
        }
    } catch (...) {
        /* noexcept 契约：投递失败仅意味着等待者保持挂起至超时/取消 */
    }
    return true;
}

WaitStatus CompletionSignal::Wait(const WaitOptions& options)
{
    /* 环境检查先于完成检查（契约 §C1）：非协程上下文一律拒绝，
     * 旧代际/停机中信号即便已完成也不得唤醒新代际协程。 */
    if (!g_bbt_tls_helper->EnableUseCo())
        return WaitStatus::InvalidContext;
    auto* co = g_bbt_tls_coroutine_co;
    if (co == nullptr)
        return WaitStatus::InvalidContext;

    if (detail::Scheduler::GetInstance()->GetRunGeneration() != m_generation)
        return WaitStatus::RuntimeUnavailable;

    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (m_completed)
            return WaitStatus::Completed;
        /* 在途计数先于窄等待占用等待位：Complete 与「完成检查 → 事件
         * 登记」之间的竞争窗口由该计数关闭（见 Complete 投递注释）。 */
        ++m_waiting;
    }

    /* 取消/超时/完成/占用仲裁全部委托 CoWaiter 窄接口：胜负在事件
     * 状态机的首个胜出触发处一次定死（唤醒原因掩码），本层不做
     * 恢复后第二遍重判，也不再自建结果 CAS。 */
    const WaitStatus status = m_waiter->Wait(options);

    {
        /* 恢复后第一动作即在同一把互斥内出账：既是 Complete 投递自旋
         * 的停止边界，也与 Complete 的置位构成 release/acquire——完成方
         * 先写的业务结果对返回 Completed 的等待者可见。 */
        std::lock_guard<std::mutex> lock(m_mtx);
        --m_waiting;
    }
    return status;
}

} // namespace bbt::coroutine
