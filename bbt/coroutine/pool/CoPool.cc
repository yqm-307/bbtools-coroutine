/** @file CoPool.cc @brief 固定数量协程池：任务提交、异常隔离与取消式停机；队列里的 Work 由提交方分配、消费者释放。 */

#include <bbt/coroutine/pool/Work.hpp>
#include <bbt/coroutine/pool/CoPool.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/syntax/SyntaxMacro.hpp>

namespace bbt::coroutine::pool {

std::shared_ptr<CoPool> CoPool::Create(int max_co)
{
    return std::make_shared<CoPool>(max_co);
}

CoPool::CoPool(int max)
    : m_max_co_num(max), m_cond(sync::CoCond::Create()), m_latch(m_max_co_num + 1) // monitor co + work co
{
    Assert(m_max_co_num > 0);
    _Start();
}

CoPool::~CoPool()
{
    // 先排空队列：delete Work 释放回调，未执行的 future 以 broken_promise 兑现，调用方不会永挂。
    Work* item = nullptr;
    while (m_works_queue.try_dequeue(item))
        delete item;

    // 再等 worker/monitor 协程真正退出。Release 末尾会再 drain 一次，那时已无消费者。
    Release();
}

int CoPool::Submit(const CoPoolWorkCallback& workfunc)
{
    // Work 由这里分配，入队后所有权归消费者（worker 或 Release 的 drain）；失败分支各自 delete。
    auto* work = new (std::nothrow) Work(workfunc);
    if (work == nullptr)
        return -1;

    // 池已停止：此后没有消费者，入队只会滞留到析构时才被释放。
    if (!m_is_running.load(std::memory_order_acquire)) {
        delete work;
        return -1;
    }

    if (!m_works_queue.enqueue(work)) {
        delete work;
        return -1;
    }

    // 这次唤醒可能落空（worker 刚 try_dequeue 失败、还没挂上 waiter），由 _MonitorCo 按队列长度补发。
    m_cond->NotifyOne();
    return 0;
}

int CoPool::Submit(CoPoolWorkCallback&& workfunc)
{
    auto* work = new (std::nothrow) Work(std::move(workfunc));
    if (work == nullptr)
        return -1;

    if (!m_is_running.load(std::memory_order_acquire)) {
        delete work;
        return -1;
    }

    if (!m_works_queue.enqueue(work)) {
        delete work;
        return -1;
    }

    m_cond->NotifyOne();
    return 0;
}

std::future<void> CoPool::SubmitWithFuture(const CoPoolWorkCallback& workfunc)
{
    // promise 随 Work 释放：未执行的任务由 delete Work 兑现 broken_promise，失败分支不必 set_exception。
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();

    auto* work = new (std::nothrow) Work(workfunc, std::move(promise));
    if (work == nullptr)
        return {};

    if (!m_is_running.load(std::memory_order_acquire)) {
        delete work;
        return {};
    }

    if (!m_works_queue.enqueue(work)) {
        delete work;
        return {};
    }

    m_cond->NotifyOne();
    return future;
}

std::future<void> CoPool::SubmitWithFuture(CoPoolWorkCallback&& workfunc)
{
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();

    auto* work = new (std::nothrow) Work(std::move(workfunc), std::move(promise));
    if (work == nullptr)
        return {};

    if (!m_is_running.load(std::memory_order_acquire)) {
        delete work;
        return {};
    }

    if (!m_works_queue.enqueue(work)) {
        delete work;
        return {};
    }

    m_cond->NotifyOne();
    return future;
}

void CoPool::SetExceptionCallback(std::function<void(std::exception_ptr)> callback)
{
    // 回调在 worker 协程里、任务异常之后执行；这里只换引用，交付侧在锁外拷贝一份再调用。
    std::lock_guard<std::mutex> lock(m_exception_cb_mtx);
    m_exception_callback = std::move(callback);
}

uint64_t CoPool::GetUnhandledExceptionCount() const noexcept
{
    return m_unhandled_exception_count.load(std::memory_order_relaxed);
}

void CoPool::_HandleWorkException(Work* work, const std::exception_ptr& eptr) noexcept
{
    // 1. future 路径优先：异常随任务交付调用方，不参与池级策略
    if (work->HasPromise()) {
        work->SetException(eptr);
        return;
    }

    // 2. 回调路径：交付异常；回调自身异常同样隔离（仅计数）
    std::function<void(std::exception_ptr)> callback{nullptr};
    {
        std::lock_guard<std::mutex> lock(m_exception_cb_mtx);
        callback = m_exception_callback;
    }

    if (callback != nullptr) {
        try {
            callback(eptr);
        } catch (...) {
            m_unhandled_exception_count.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    // 3. 忽略路径：记录
    m_unhandled_exception_count.fetch_add(1, std::memory_order_relaxed);
}

void CoPool::Release()
{
    m_is_running.store(false, std::memory_order_release);
    /* 没有第二条逃生条件（不再依赖 Scheduler::IsRunning()）：运行时不设停机；等待必须有
     * m_running_co_num 递减的实际进展，否则是卡死而不是慢。 */
    while (m_running_co_num != 0) {
        m_cond->NotifyAll();

        if (g_bbt_tls_helper->EnableUseCo())
            bbtco_sleep(5);
        else
            std::this_thread::sleep_for(bbt::core::clock::ms(5));
    }

    m_latch.Wait();

    /* worker 全部退出后再 drain：与消费不竞态，每个 Work 恰处理一次；delete Work 释放
     * promise，未执行的 SubmitWithFuture 以 broken_promise 兑现。 */
    Work* item = nullptr;
    while (m_works_queue.try_dequeue(item))
        delete item;
}

void CoPool::_Start()
{
    /* 启动工作协程 */
    for (int i = 0; i < m_max_co_num; ++i)
        bbtco_desc("work co")[=]()
        {
            // Down 在 _WorkCo 返回之后：池退出以协程真正结束为准，而不是 m_is_running 变 false。
            _WorkCo();
            m_latch.Down();
        };

    /* 启动监视协程 */
    bbtco_desc("monitor co")[=]()
    {
        _MonitorCo();
        m_latch.Down();
    };
}

void CoPool::_WorkCo()
{
    // 计数必须在循环外加减：Release 靠 m_running_co_num 归零判断 worker 全部退出。
    m_running_co_num++;

    while (m_is_running.load(std::memory_order_acquire)) {
        Work* work = nullptr;
        if (!m_works_queue.try_dequeue(work)) {
            m_cond->Wait();
            continue;
        }

        // 异常隔离：worker 协程吞掉任务异常并记录/交付，保证池持续工作，
        // 且本协程不因异常逃逸而终结（否则 m_latch 永不 Down，Release 挂起）
        try {
            work->Invoke();
            work->SetFinished();
        } catch (...) {
            _HandleWorkException(work, std::current_exception());
        }

        delete work;
    }

    m_running_co_num--;
}

void CoPool::_MonitorCo()
{
    // 补发唤醒的兜底：入队与 NotifyOne 之间有窗口（worker 刚 try_dequeue 失败、还没挂上
    // waiter），这次唤醒会丢、任务留在队列里，Submit 侧无法重试，所以这里按队列长度补发；
    // NotifyOne 返回非 0 说明没有挂起的 worker，提前退出。
    m_running_co_num++;

    while (m_is_running.load(std::memory_order_acquire)) {

        auto remain_works_num = m_works_queue.size_approx();

        for (int i = 0; i < remain_works_num; ++i) {
            if (m_cond->NotifyOne() != 0)
                break;
        }

        bbtco_sleep(1);
    }

    m_running_co_num--;
}

}
