#include <bbt/coroutine/detail/DnsResolver.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>

namespace bbt::coroutine::detail
{

DnsResolver* DnsResolver::GetInstance()
{
    /* 进程寿命：本对象持一个永不退出的 worker 线程，其 std::thread 句柄
     * 必须在静态退出期保持存活（析构 joinable thread 会 std::terminate）。
     * 因此故意让实例泄漏，而不是 detach 线程。 */
    static DnsResolver* _inst = new DnsResolver();
    return _inst;
}

bool DnsResolver::Enqueue(Job job)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.size() >= kMaxPendingJobs)
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
     * 这里执行的 libc 阻塞调用只占用本线程，且其内部再进 hook 时走直通路径。
     * 进程寿命：没有停机路径，本循环不设退出条件。 */
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this](){ return !m_queue.empty(); });
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
     * 入队失败（队列满）时在同一回调里 Notify 唤醒自己，调用方凭栈上结果的
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

sync::CombinedWaitStatus DnsResolver::AwaitBounded(
    const std::function<void()>& work,
    const sync::CombinedWaitOptions& options)
{
    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(),
                   "DnsResolver::AwaitBounded must be in coroutine!");
    auto waiter = sync::CoWaiter::Create();
    bool enqueued = false;
    bool enqueue_attempted = false;
    const auto status = waiter->Wait(options, [this, waiter, work,
                                               &enqueue_attempted, &enqueued]() {
        enqueue_attempted = true;
        enqueued = Enqueue(Job{work, waiter});
        if (!enqueued)
            waiter->Notify();
        return true;
    });
    if (enqueue_attempted && !enqueued)
        return sync::CombinedWaitStatus::RuntimeUnavailable;
    return status;
}

} // namespace bbt::coroutine::detail
