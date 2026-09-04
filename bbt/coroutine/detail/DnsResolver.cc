#include <bbt/coroutine/detail/DnsResolver.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>

namespace bbt::coroutine::detail
{

DnsResolver* DnsResolver::GetInstance()
{
    static DnsResolver _inst;
    return &_inst;
}

bool DnsResolver::Enqueue(Job job)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping)
            return false;
        m_queue.push(std::move(job));
        if (!m_thread) {
            try {
                m_thread.reset(new std::thread([this](){ _Worker(); }));
            } catch (...) {
                // 线程创建失败：撤回任务，调用方按入队失败路径（默认错误码）收尾
                m_queue.pop();
                return false;
            }
        }
    }
    m_cv.notify_one();
    return true;
}

void DnsResolver::_Worker()
{
    /* worker 不是协程线程：LocalThread 默认 EnableUseCo()==false，
     * 这里执行的 libc 阻塞调用只占用本线程，且其内部再进 hook 时走直通路径 */
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this](){ return m_stopping || !m_queue.empty(); });
            if (m_queue.empty())
                return; // 置停后不再有入队，队列被 Stop 清空即退出
            job = std::move(m_queue.front());
            m_queue.pop();
        }

        if (job.work)
            job.work();
        /* 时序约束：Notify 从非协程线程调用，CoWaiter/CoPollEvent 状态机线程安全 */
        if (job.waiter)
            job.waiter->Notify();
    }
}

int DnsResolver::Await(const std::function<void()>& work)
{
    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "DnsResolver::Await must be in coroutine!");

    auto waiter = sync::CoWaiter::Create();
    bool enqueued = false;

    /**
     * 时序约束：WaitWithCallback 的回调在协程 Yield 之后（Processer Resume 内）执行，
     * 所以在回调里 Enqueue——worker 的 Notify 不可能早于协程挂起，不会丢唤醒。
     * 入队失败（池已停止）时在同一回调里 Notify 唤醒自己，调用方凭栈上结果的
     * 默认错误值返回失败。
     */
    waiter->WaitWithCallback([this, &enqueued, waiter, &work]() {
        enqueued = Enqueue(Job{work, waiter});
        if (!enqueued)
            waiter->Notify();
        return true;
    });

    return enqueued ? 0 : -1;
}

void DnsResolver::Stop()
{
    std::queue<Job> drained;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping)
            return; // 幂等：重复调用直接返回（调用点 Scheduler::Stop 是串行的）
        m_stopping = true;
        drained.swap(m_queue);
    }
    m_cv.notify_all();

    /* 排队未执行的任务：不跑 work（结果保持调用方预置的失败值），只唤醒协程，
     * 让其在 Processer 还存活时返回错误路径 */
    while (!drained.empty())
    {
        Job job = std::move(drained.front());
        drained.pop();
        if (job.waiter)
            job.waiter->Notify();
    }

    /* in-flight 任务：worker 正在 libc 里，join 等它返回并 Notify；
     * 返回后不存在任何线程还会触碰调用方协程栈——Scheduler::Stop 依赖这一点
     * （必须在本函数之后才允许销毁 Processer/协程栈） */
    if (m_thread) {
        if (m_thread->joinable())
            m_thread->join();
        m_thread.reset();
    }
}

} // namespace bbt::coroutine::detail
