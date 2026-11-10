#include <bbt/coroutine/pool/Work.hpp>
#include <bbt/coroutine/pool/CoPool.hpp>
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Stack.hpp>
#include <bbt/coroutine/detail/StackPool.hpp>
#include <bbt/coroutine/syntax/SyntaxMacro.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <limits>
#include <thread>
#include <utility>

namespace bbt::coroutine::pool
{

namespace {
using ClassKey = std::pair<size_t, bool>;

constexpr int kShutdownPollMs = 1;
} // namespace

/* 一个尺寸类的运行态：路由键、固定 worker 数、有界队列与它自己的等待原语。
 * 每类独立 CoCond，避免唤醒落到别的类的 worker 上白白空转。 */
struct CoPool::ClassState {
    StackRequest request{0, false};
    ClassKey key{0, false};
    size_t worker_num{0};
    size_t capacity{0};
    std::mutex mutex;
    std::deque<Work*> queue;
    sync::CoCond::SPtr cond{nullptr};
};

std::shared_ptr<CoPool> CoPool::Create(int max_co)
{
    return std::make_shared<CoPool>(max_co);
}

std::shared_ptr<CoPool> CoPool::Create(const CoPoolOptions& options) {
    return std::make_shared<CoPool>(options);
}

CoPool::CoPool(int max) : m_max_co_num(max),
                          m_cond(sync::CoCond::Create()) {
    Assert(m_max_co_num > 0);
    _Start();
}

CoPool::CoPool(const CoPoolOptions& options) : m_cond(sync::CoCond::Create()) {
    _StartRouted(options);
}

CoPool::~CoPool() {
    // 滞留任务由 Release 统一删除（带 future 的任务在这里兑现 broken_promise）。
    // 析构早于显式关闭时也先释放节点，避免裸 Work 泄漏。
    _DrainQueues();
    try {
        Release();
    } catch (...) {
        // 在池自身 worker 内销毁池是自等待编程错误：没有可等待的执行域，也不能
        // 从析构抛出。调用方必须保证池活过全部 worker（见类注释）。
    }
}

int CoPool::Submit(const CoPoolWorkCallback& workfunc)
{
    /* 尺寸路由池没有默认尺寸：不替调用方挑一个类，直接拒绝。 */
    if (!m_classes.empty())
        return -1;

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
    if (!m_classes.empty())
        return -1;

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

CoPoolSubmitStatus CoPool::Submit(StackRequest request, const CoPoolWorkCallback& workfunc) {
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

CoPoolSubmitStatus CoPool::Submit(StackRequest request, CoPoolWorkCallback&& workfunc) {
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
    /* 尺寸路由池没有默认尺寸：future 提交同样必须显式带栈规格。 */
    if (!m_classes.empty())
        return {};

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
    if (!m_classes.empty())
        return {};

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

void CoPool::_RunWork(Work* work) noexcept {
    // 异常隔离：worker 协程吞掉任务异常并记录/交付，保证池持续工作，
    // 且本协程不因异常逃逸而终结（否则退出账目永远凑不齐，Release 挂起）。
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

    m_is_running.store(false, std::memory_order_release);
    _WaitWorkersExit();
    _DrainQueues();
}

void CoPool::_WaitWorkersExit() {
    /* Co 域必须让出而不是阻塞线程：池可能运行在唯一的 Processer 上，
     * 原生等待会让被等的 worker 永远得不到执行。 */
    const bool in_co = (GetLocalCoroutineId() != 0);
    while (m_exited_workers.load(std::memory_order_acquire) <
           m_created_workers.load(std::memory_order_acquire)) {
        if (m_cond != nullptr)
            m_cond->NotifyAll();
        for (const auto& state : m_classes)
            state->cond->NotifyAll();

        if (in_co)
            bbtco_sleep(kShutdownPollMs);
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(kShutdownPollMs));
    }
}

void CoPool::_DrainQueues() noexcept {
    Work* item = nullptr;
    while (m_works_queue.try_dequeue(item))
        delete item;

    for (auto& state : m_classes) {
        std::deque<Work*> drained;
        try {
            std::lock_guard<std::mutex> lock(state->mutex);
            drained.swap(state->queue);
        } catch (...) {
            continue;
        }
        for (auto* work : drained)
            delete work;
    }
}

bool CoPool::_IsInPoolWorker() const {
    const auto id = GetLocalCoroutineId();
    if (id == 0)
        return false;

    std::lock_guard<std::mutex> lock(m_worker_ids_mutex);
    return std::find(m_worker_ids.begin(), m_worker_ids.end(), id) != m_worker_ids.end();
}

void CoPool::_RecordWorkerId() {
    const auto id = GetLocalCoroutineId();
    if (id == 0)
        return;

    std::lock_guard<std::mutex> lock(m_worker_ids_mutex);
    m_worker_ids.push_back(id);
}

CoPool::ClassState* CoPool::_FindClass(StackRequest request) const {
    if (!request.usable_bytes)
        return nullptr;

    size_t usable = 0;
    try {
        usable = detail::Stack::NormalizeSize(request.usable_bytes);
    } catch (...) {
        return nullptr; // 尺寸溢出/对齐失败没有匹配类，按显式拒绝处理
    }

    const ClassKey key{usable, request.protect};
    for (const auto& state : m_classes)
        if (state->key == key)
            return state.get();
    return nullptr;
}

CoPoolSubmitStatus CoPool::_Enqueue(ClassState& state, Work* work) {
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        /* 与 Release 的排空在同一把类锁下判定：封口之后到达的提交不会被
         * 漏在队列里，封口之前入队的任务一定被排空。 */
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

    state.cond->NotifyOne();
    return CoPoolSubmitStatus::Accepted;
}

void CoPool::_Start()
{
    const StackRequest request{g_bbt_coroutine_config->RuntimeSnapshot().stack_size,
                               g_bbt_coroutine_config->RuntimeSnapshot().stack_protect};
    try {
        /* 启动工作协程 */
        for (int i = 0; i < m_max_co_num; ++i)
            _StartWorker(request, nullptr, "work co");

        /* 启动监视协程 */
        _StartMonitor(request, "monitor co");
    } catch (...) {
        _AbortStart();
        throw;
    }
}

void CoPool::_StartWorker(StackRequest request, ClassState* state, const char* desc) {
    // 先记账再登记：worker 可能在登记返回后立刻开始/结束，exited 不能超过 created。
    ++m_created_workers;
    try {
        auto body = [this, state]() {
            _RecordWorkerId();
            /* worker 内部失败不能让退出账目丢失，否则 Release 永远等不齐。 */
            try {
                if (state != nullptr)
                    _WorkCoRoute(*state);
                else
                    _WorkCo();
            } catch (...) {
            }
            m_exited_workers.fetch_add(1, std::memory_order_acq_rel);
        };
        g_scheduler->RegistCoroutineTask(request, body, {}, desc);
    } catch (...) {
        --m_created_workers;
        throw;
    }
}

void CoPool::_StartMonitor(StackRequest request, const char* desc) {
    ++m_created_workers;
    try {
        auto body = [this]() {
            _RecordWorkerId();
            try {
                _MonitorCo();
            } catch (...) {
            }
            m_exited_workers.fetch_add(1, std::memory_order_acq_rel);
        };
        g_scheduler->RegistCoroutineTask(request, body, {}, desc);
    } catch (...) {
        --m_created_workers;
        throw;
    }
}

void CoPool::_StartRouted(const CoPoolOptions& options) {
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

        // 0 字节在这里就抛 invalid_argument；规范化后再判重，避免 12 KiB 与 12 KiB+1 同桶。
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
        state->cond = sync::CoCond::Create();
        classes.push_back(std::move(state));
    }

    /* 创建前容量校验：worker 栈与 monitor 栈都要在当前 StackPool 策略里真正可接纳，
     * 不通过就不留半个池。查询是只读快照，真实分配仍可能失败，失败走下面的回滚。 */
    for (const auto& state : classes)
        if (!g_bbt_stackpoll->CanAdmit(state->request, state->worker_num))
            throw std::runtime_error("CoPool: class workers exceed the stack budget");

    const StackRequest monitor{g_bbt_coroutine_config->RuntimeSnapshot().stack_size,
                               g_bbt_coroutine_config->RuntimeSnapshot().stack_protect};
    if (!g_bbt_stackpoll->CanAdmit(monitor, 1))
        throw std::runtime_error("CoPool: monitor stack exceeds the stack budget");

    m_classes = std::move(classes);

    try {
        for (const auto& state : m_classes)
            for (size_t i = 0; i != state->worker_num; ++i)
                _StartWorker(state->request, state.get(), "copool worker");

        _StartMonitor(monitor, "copool monitor");
    } catch (...) {
        _AbortStart();
        throw;
    }
}

void CoPool::_AbortStart() noexcept {
    // 已登记的 worker 看到封口后会立即退出并归还栈；等到它们真实离开，
    // 调用方的析构（shared_ptr 未交付）才不会撕掉在跑协程引用的成员。
    m_is_running.store(false, std::memory_order_release);
    _WaitWorkersExit();
}

void CoPool::_WorkCo() {
    while (m_is_running.load(std::memory_order_acquire)) {
        Work* work = nullptr;
        if (!m_works_queue.try_dequeue(work)) {
            m_cond->Wait();
            continue;
        }

        _RunWork(work);
        delete work;
    }
}

void CoPool::_WorkCoRoute(ClassState& state) {
    while (m_is_running.load(std::memory_order_acquire)) {
        Work* work = nullptr;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (!state.queue.empty()) {
                work = state.queue.front();
                state.queue.pop_front();
            }
        }

        if (work == nullptr) {
            state.cond->Wait();
            continue;
        }

        _RunWork(work);
        delete work;
    }
}

void CoPool::_MonitorCo()
{
    /**
     * 每隔一段时间，监视协程会检测是否有“摸鱼”的work协程，并将其
     * 唤醒起来执行任务
     */
    while (m_is_running.load(std::memory_order_acquire)) {
        if (m_classes.empty()) {
            auto remain_works_num = m_works_queue.size_approx();

            for (size_t i = 0; i < remain_works_num; ++i) {
                /* 唤醒失败，说明没有阻塞的co */
                if (m_cond->NotifyOne() != 0)
                    break;
            }
        } else {
            for (const auto& state : m_classes) {
                size_t queued = 0;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    queued = state->queue.size();
                }
                for (size_t i = 0; i < queued; ++i) {
                    if (state->cond->NotifyOne() != 0)
                        break;
                }
            }
        }

        bbtco_sleep(1);
    }
}

} // namespace bbt::coroutine::pool
