/**
 * @file CoPool.cc
 * @brief 固定数量协程池：任务提交、异常隔离与取消式停机；队列里的 Work 由提交方分配、消费者释放。
 */
#include <bbt/coroutine/pool/Work.hpp>
#include <bbt/coroutine/pool/CoPool.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Stack.hpp>
#include <bbt/coroutine/detail/StackPool.hpp>
#include <bbt/coroutine/syntax/SyntaxMacro.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>
#include <utility>

namespace bbt::coroutine::pool {

namespace {
using ClassKey = std::pair<size_t, bool>;

constexpr int kShutdownPollMs = 1;
}

/**
 * 一个持久唤醒信号：sequence 记录每次入队/关闭，waiters 只保存当前挂起的 worker。
 */
struct CoPool::WaitSignal {
    std::mutex mutex;
    std::deque<sync::CoWaiter::SPtr> waiters;
    std::atomic_uint64_t sequence{0};
};

/**
 * 一个尺寸类的运行态：路由键、固定 worker 数、有界队列与它自己的等待原语。
 */
struct CoPool::ClassState {
    StackRequest request{0, false};
    ClassKey key{0, false};
    size_t worker_num{0};
    size_t capacity{0};
    std::mutex mutex;
    std::deque<Work*> queue;
    std::unique_ptr<WaitSignal> signal;
};

std::shared_ptr<CoPool> CoPool::Create(int max_co)
{
    return std::make_shared<CoPool>(max_co);
}

std::shared_ptr<CoPool> CoPool::Create(const CoPoolOptions& options)
{
    return std::make_shared<CoPool>(options);
}

CoPool::CoPool(int max) : m_max_co_num(max), m_work_signal(std::make_unique<WaitSignal>())
{
    Assert(m_max_co_num > 0);
    _Start();
}

CoPool::CoPool(const CoPoolOptions& options) : m_work_signal(std::make_unique<WaitSignal>())
{
    _StartRouted(options);
}

CoPool::~CoPool()
{
    try {
        Release();
    } catch (...) {
        /**
         * Release 失败时 worker 仍可能持有 this；继续析构会把它们变成悬空访问。
         * 析构不能向外抛出，只能终止进程保留内存安全。
         */
        std::terminate();
    }
}

int CoPool::Submit(const CoPoolWorkCallback& workfunc)
{

    /**
     * 尺寸路由池没有默认尺寸：不替调用方挑一个类，直接拒绝。
     */
    if (!m_classes.empty())
        return -1;

    /**
     * Work 由这里分配，入队后所有权归消费者（worker 或 Release 的 drain）；失败分支各自 delete。
     */
    auto* work = new (std::nothrow) Work(workfunc);
    if (work == nullptr)
        return -1;

    if (!_EnqueueDefault(work)) {
        delete work;
        return -1;
    }

    _NotifyOne(*m_work_signal);
    return 0;
}

int CoPool::Submit(CoPoolWorkCallback&& workfunc)
{
    if (!m_classes.empty())
        return -1;

    auto* work = new (std::nothrow) Work(std::move(workfunc));
    if (work == nullptr)
        return -1;

    if (!_EnqueueDefault(work)) {
        delete work;
        return -1;
    }

    _NotifyOne(*m_work_signal);
    return 0;
}

CoPoolSubmitStatus CoPool::Submit(StackRequest request, const CoPoolWorkCallback& workfunc)
{
    auto* state = _FindClass(request);
    if (state == nullptr)
        return CoPoolSubmitStatus::NoExactClass;

    Work* work = nullptr;
    try {
        work = new (std::nothrow) Work(workfunc);
    } catch (...) {
        return CoPoolSubmitStatus::AllocationFailed;
    }
    if (work == nullptr)
        return CoPoolSubmitStatus::AllocationFailed;

    const auto status = _Enqueue(*state, work);
    if (status != CoPoolSubmitStatus::Accepted)
        delete work;
    return status;
}

CoPoolSubmitStatus CoPool::Submit(StackRequest request, CoPoolWorkCallback&& workfunc)
{
    auto* state = _FindClass(request);
    if (state == nullptr)
        return CoPoolSubmitStatus::NoExactClass;

    Work* work = nullptr;
    try {
        work = new (std::nothrow) Work(std::move(workfunc));
    } catch (...) {
        return CoPoolSubmitStatus::AllocationFailed;
    }
    if (work == nullptr)
        return CoPoolSubmitStatus::AllocationFailed;

    const auto status = _Enqueue(*state, work);
    if (status != CoPoolSubmitStatus::Accepted)
        delete work;
    return status;
}

std::future<void> CoPool::SubmitWithFuture(const CoPoolWorkCallback& workfunc)
{

    /**
     * 尺寸路由池没有默认尺寸：future 提交同样必须显式带栈规格。
     */
    if (!m_classes.empty())
        return {};

    /**
     * promise 随 Work 释放：未执行的任务由 delete Work 兑现 broken_promise，失败分支不必 set_exception。
     */
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();

    auto* work = new (std::nothrow) Work(workfunc, std::move(promise));
    if (work == nullptr)
        return {};

    if (!_EnqueueDefault(work)) {
        delete work;
        return {};
    }

    _NotifyOne(*m_work_signal);
    return future;
}

std::future<void> CoPool::SubmitWithFuture(CoPoolWorkCallback&& workfunc)
{
    if (!m_classes.empty())
        return {};

    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();

    auto* work = new (std::nothrow) Work(std::move(workfunc), std::move(promise));
    if (work == nullptr)
        return {};

    if (!_EnqueueDefault(work)) {
        delete work;
        return {};
    }

    _NotifyOne(*m_work_signal);
    return future;
}

void CoPool::SetExceptionCallback(std::function<void(std::exception_ptr)> callback)
{

    /**
     * 回调在 worker 协程里、任务异常之后执行；这里只换引用，交付侧在锁外拷贝一份再调用。
     */
    std::lock_guard<std::mutex> lock(m_exception_cb_mtx);
    m_exception_callback = std::move(callback);
}

uint64_t CoPool::GetUnhandledExceptionCount() const noexcept
{
    return m_unhandled_exception_count.load(std::memory_order_relaxed);
}

void CoPool::_HandleWorkException(Work* work, const std::exception_ptr& eptr) noexcept
{

    /**
     * 1. future 路径优先：异常随任务交付调用方，不参与池级策略
     */
    if (work->HasPromise()) {
        work->SetException(eptr);
        return;
    }

    /**
     * 2. 回调路径：交付异常；回调自身异常同样隔离（仅计数）
     */
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

    /**
     * 3. 忽略路径：记录
     */
    m_unhandled_exception_count.fetch_add(1, std::memory_order_relaxed);
}

void CoPool::_HandleWorkerException(const std::exception_ptr& eptr) noexcept
{
    /**
     * Wait/condition infrastructure failure invalidates the fixed-worker guarantee.
     * Stop admission first so no caller can enqueue work after the failure is visible.
     */
    _Seal();
    m_unhandled_exception_count.fetch_add(1, std::memory_order_relaxed);

    /**
     * This path is noexcept and deliberately does not invoke the user callback: callback
     * delivery is a task-exception contract, while a failed wakeup primitive must only
     * make the pool fail closed and observable through submission/release state.
     */
    (void)eptr;
    try {
        if (m_work_signal != nullptr)
            _NotifyAll(*m_work_signal);
    } catch (...) {
    }
    for (const auto& state : m_classes) {
        try {
            if (state->signal != nullptr)
                _NotifyAll(*state->signal);
        } catch (...) {
        }
    }
}

void CoPool::_RunWork(Work* work) noexcept
{

    /**
     * 异常隔离：worker 协程吞掉任务异常并记录/交付，保证池持续工作，
     * 且本协程不因异常逃逸而终结（否则退出账目永远凑不齐，Release 挂起）。
     */
    try {
        work->Invoke();
        work->SetFinished();
    } catch (...) {
        _HandleWorkException(work, std::current_exception());
    }
}

void CoPool::Release()
{
    if (_IsInPoolWorker())
        throw std::logic_error("CoPool::Release cannot wait for its own worker");

    _Seal();
    _WaitWorkersExit();
    _DrainQueues();
}

void CoPool::_Seal() noexcept
{
    std::lock_guard<std::mutex> lock(m_work_queue_mutex);
    m_is_running.store(false, std::memory_order_release);
}

void CoPool::_WaitWorkersExit()
{

    /**
     * Co 域必须让出而不是阻塞线程：池可能运行在唯一的 Processer 上，
     * 原生等待会让被等的 worker 永远得不到执行。
     */
    const bool in_co = (GetLocalCoroutineId() != 0);
    while (m_exited_workers.load(std::memory_order_acquire) < m_created_workers.load(std::memory_order_acquire)) {
        if (m_work_signal != nullptr)
            _NotifyAll(*m_work_signal);
        for (const auto& state : m_classes)
            _NotifyAll(*state->signal);

        if (in_co) {
            /**
             * 取消态的 bbtco_sleep 会直接返回，不会让出当前 Processer；构造回滚
             * 必须暂时采用无条件让出，否则单 Processer 下 worker 永远得不到运行。
             */
            if (g_bbt_tls_coroutine_co != nullptr && g_bbt_tls_coroutine_co->IsCancelRequested())
                g_bbt_tls_coroutine_co->YieldAndPushGCoQueue();
            else
                bbtco_sleep(kShutdownPollMs);
        } else
            std::this_thread::sleep_for(std::chrono::milliseconds(kShutdownPollMs));
    }
}

void CoPool::_DrainQueues() noexcept
{
    /**
     * _Seal 已与默认入队串行化，不会再有新任务；释放 Work 可能析构用户捕获，不能持队列锁。
     */
    Work* item = nullptr;
    while (m_works_queue.try_dequeue(item))
        delete item;

    for (auto& state : m_classes) {
        for (;;) {
            Work* work = nullptr;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->queue.empty())
                    break;
                work = state->queue.front();
                state->queue.pop_front();
            }
            delete work;
        }
    }
}

bool CoPool::_IsInPoolWorker() const
{
    const auto id = GetLocalCoroutineId();
    if (id == 0)
        return false;

    std::lock_guard<std::mutex> lock(m_worker_ids_mutex);
    return std::find(m_worker_ids.begin(), m_worker_ids.end(), id) != m_worker_ids.end();
}

void CoPool::_RecordWorkerId()
{
    const auto id = GetLocalCoroutineId();
    if (id == 0)
        return;

    std::lock_guard<std::mutex> lock(m_worker_ids_mutex);
    m_worker_ids.push_back(id);
}

CoPool::ClassState* CoPool::_FindClass(StackRequest request) const
{
    if (!request.usable_bytes)
        return nullptr;

    size_t usable = 0;
    try {
        usable = detail::Stack::NormalizeSize(request.usable_bytes);
    } catch (...) {

        /**
         * 尺寸溢出/对齐失败没有匹配类，按显式拒绝处理
         */
        return nullptr;
    }

    const ClassKey key{usable, request.protect};
    for (const auto& state : m_classes)
        if (state->key == key)
            return state.get();
    return nullptr;
}

bool CoPool::_EnqueueDefault(Work* work)
{
    std::lock_guard<std::mutex> lock(m_work_queue_mutex);
    return m_is_running.load(std::memory_order_acquire) && m_works_queue.enqueue(work);
}

CoPoolSubmitStatus CoPool::_Enqueue(ClassState& state, Work* work)
{
    {
        std::lock_guard<std::mutex> lock(state.mutex);

        /**
         * 与 Release 的排空在同一把类锁下判定：封口之后到达的提交不会被
         * 漏在队列里，封口之前入队的任务一定被排空。
         */
        if (!m_is_running.load(std::memory_order_acquire))
            return CoPoolSubmitStatus::Released;
        if (state.queue.size() >= state.capacity)
            return CoPoolSubmitStatus::QueueFull;
        try {
            state.queue.push_back(work);
        } catch (...) {
            return CoPoolSubmitStatus::AllocationFailed;
        }
    }

    _NotifyOne(*state.signal);
    return CoPoolSubmitStatus::Accepted;
}

void CoPool::_Start()
{
    const StackRequest request{g_bbt_coroutine_config->RuntimeSnapshot().stack_size,
                               g_bbt_coroutine_config->RuntimeSnapshot().stack_protect};
    m_worker_ids.reserve(static_cast<size_t>(m_max_co_num));
    try {

        /**
         * 启动工作协程
         */
        for (int i = 0; i < m_max_co_num; ++i)
            _StartWorker(request, nullptr, "work co");

    } catch (...) {
        _AbortStart();
        throw;
    }
}

void CoPool::_StartWorker(StackRequest request, ClassState* state, const char* desc)
{

    /**
     * 先记账再登记：worker 可能在登记返回后立刻开始/结束，exited 不能超过 created。
     */
    ++m_created_workers;
    try {
        auto body = [this, state]() {
            try {
                _RecordWorkerId();
                if (state != nullptr)
                    _WorkCoRoute(*state);
                else
                    _WorkCo();
            } catch (...) {
                _HandleWorkerException(std::current_exception());
            }
            m_exited_workers.fetch_add(1, std::memory_order_acq_rel);
        };
        g_scheduler->RegistCoroutineTask(request, body, {}, desc);
    } catch (...) {
        --m_created_workers;
        throw;
    }
}

void CoPool::_StartRouted(const CoPoolOptions& options)
{
    if (options.classes.empty())
        throw std::invalid_argument("CoPool: at least one stack class is required");

    std::vector<std::unique_ptr<ClassState>> classes;
    classes.reserve(options.classes.size());
    size_t total_workers = 0;
    for (const auto& config : options.classes) {
        if (!config.worker_num)
            throw std::invalid_argument("CoPool: class worker count must be positive");
        if (!config.queue_capacity)
            throw std::invalid_argument("CoPool: class queue capacity must be positive");

        /**
         * 0 字节在这里就抛 invalid_argument；规范化后再判重，避免 12 KiB 与 12 KiB+1 同桶。
         */
        const size_t usable = detail::Stack::NormalizeSize(config.request.usable_bytes);
        const ClassKey key{usable, config.request.protect};
        for (const auto& other : classes)
            if (other->key == key)
                throw std::invalid_argument("CoPool: duplicate stack class");

        if (total_workers > std::numeric_limits<size_t>::max() - config.worker_num)
            throw std::length_error("CoPool: worker count overflow");
        total_workers += config.worker_num;

        auto state = std::make_unique<ClassState>();
        state->request = StackRequest{usable, config.request.protect};
        state->key = key;
        state->worker_num = config.worker_num;
        state->capacity = config.queue_capacity;
        state->signal = std::make_unique<WaitSignal>();
        classes.push_back(std::move(state));
    }

    /**
     * 创建前容量校验：worker 栈要在当前 StackPool 策略里真正可接纳，
     * 不通过就不留半个池。查询是只读快照，真实分配仍可能失败，失败走下面的回滚。
     */
    for (const auto& state : classes)
        if (!g_bbt_stackpoll->CanAdmit(state->request, state->worker_num))
            throw std::runtime_error("CoPool: class workers exceed the stack budget");

    if (total_workers == std::numeric_limits<size_t>::max())
        throw std::length_error("CoPool: worker count overflow");
    m_worker_ids.reserve(total_workers);
    m_classes = std::move(classes);

    try {
        for (const auto& state : m_classes)
            for (size_t i = 0; i != state->worker_num; ++i)
                _StartWorker(state->request, state.get(), "copool worker");

    } catch (...) {
        _AbortStart();
        throw;
    }
}

void CoPool::_AbortStart() noexcept
{

    /**
     * 已登记的 worker 看到封口后会立即退出并归还栈；等到它们真实离开，
     * 调用方的析构（shared_ptr 未交付）才不会撕掉在跑协程引用的成员。
     */
    _Seal();
    _WaitWorkersExit();
}

void CoPool::_WaitForWork(WaitSignal& signal, uint64_t observed_sequence)
{
    auto waiter = sync::CoWaiter::Create();
    waiter->WaitWithCallback([this, &signal, waiter, observed_sequence]() {
        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(signal.mutex);
            signal.waiters.push_back(waiter);
            if (!m_is_running.load(std::memory_order_acquire)
                || signal.sequence.load(std::memory_order_acquire) != observed_sequence) {
                signal.waiters.pop_back();
                notify = true;
            }
        }
        if (notify)
            waiter->Notify();
        return true;
    });
}

void CoPool::_NotifyOne(WaitSignal& signal) noexcept
{
    sync::CoWaiter::SPtr waiter;
    {
        std::lock_guard<std::mutex> lock(signal.mutex);
        signal.sequence.fetch_add(1, std::memory_order_release);
        if (!signal.waiters.empty()) {
            waiter = signal.waiters.front();
            signal.waiters.pop_front();
        }
    }

    while (waiter != nullptr) {
        try {
            if (waiter->Notify() == 0)
                return;
        } catch (...) {
        }
        std::lock_guard<std::mutex> lock(signal.mutex);
        if (signal.waiters.empty())
            return;
        waiter = signal.waiters.front();
        signal.waiters.pop_front();
    }
}

void CoPool::_NotifyAll(WaitSignal& signal) noexcept
{
    std::deque<sync::CoWaiter::SPtr> waiters;
    {
        std::lock_guard<std::mutex> lock(signal.mutex);
        signal.sequence.fetch_add(1, std::memory_order_release);
        waiters.swap(signal.waiters);
    }
    for (const auto& waiter : waiters) {
        try {
            waiter->Notify();
        } catch (...) {
        }
    }
}

void CoPool::_WorkCo()
{
    while (m_is_running.load(std::memory_order_acquire)) {
        const auto observed_sequence = m_work_signal->sequence.load(std::memory_order_acquire);
        Work* work = nullptr;
        if (!m_works_queue.try_dequeue(work)) {
            _WaitForWork(*m_work_signal, observed_sequence);
            continue;
        }

        _RunWork(work);
        delete work;
    }
}

void CoPool::_WorkCoRoute(ClassState& state)
{
    while (m_is_running.load(std::memory_order_acquire)) {
        const auto observed_sequence = state.signal->sequence.load(std::memory_order_acquire);
        Work* work = nullptr;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (!state.queue.empty()) {
                work = state.queue.front();
                state.queue.pop_front();
            }
        }

        if (work == nullptr) {
            _WaitForWork(*state.signal, observed_sequence);
            continue;
        }

        _RunWork(work);
        delete work;
    }
}

}
