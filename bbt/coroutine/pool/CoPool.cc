#include <bbt/coroutine/pool/Work.hpp>
#include <bbt/coroutine/pool/CoPool.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/syntax/SyntaxMacro.hpp>


namespace bbt::coroutine::pool
{

std::shared_ptr<CoPool> CoPool::Create(int max_co)
{
    return std::make_shared<CoPool>(max_co);
}

CoPool::CoPool(int max):
    m_max_co_num(max),
    m_cond(sync::CoCond::Create()),
    m_latch(m_max_co_num + 1) // monitor co + work co
{
    Assert(m_max_co_num > 0);
    _Start();
}

CoPool::~CoPool()
{
    // 先释放所有任务
    Work* item = nullptr;
    while (m_works_queue.try_dequeue(item))
        delete item;

    // 释放所有coroutine
    Release();
}

int CoPool::Submit(const CoPoolWorkCallback& workfunc)
{
    auto* work = new (std::nothrow) Work(workfunc);
    if (work == nullptr)
        return -1;

    // 池已停止：拒绝入队，避免任务滞留队列无人消费
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
    while (m_running_co_num != 0 && g_scheduler->IsRunning()) {
        m_cond->NotifyAll();

        if (g_bbt_tls_helper->EnableUseCo())
            bbtco_sleep(5);
        else
            std::this_thread::sleep_for(bbt::core::clock::ms(5));
    } 

    if (g_scheduler->IsRunning()) {
        m_latch.Wait();
    }
}

void CoPool::_Start()
{
    /* 启动工作协程 */
    for (int i = 0; i < m_max_co_num; ++i)
        bbtco_desc("work co") [=](){
            _WorkCo();
            m_latch.Down();
        };

    /* 启动监视协程 */
    bbtco_desc("monitor co") [=](){
        _MonitorCo();
        m_latch.Down();
    };
}

void CoPool::_WorkCo()
{
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
    /**
     * 每隔一段时间，监视协程会检测是否有“摸鱼”的work协程，并将其
     * 唤醒起来执行任务
     */
    m_running_co_num++;

    while (m_is_running.load(std::memory_order_acquire)) {
        
        auto remain_works_num = m_works_queue.size_approx();

        for (int i = 0; i < remain_works_num; ++i) {
            /* 唤醒失败，说明没有阻塞的co */
            if (m_cond->NotifyOne() != 0)
                break;
        }

        bbtco_sleep(1);
    }

    m_running_co_num--;
}

}
