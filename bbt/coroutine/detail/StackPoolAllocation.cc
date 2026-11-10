/** @file StackPoolAllocation.cc
 * @brief 精确栈预算与维护流交付；不拥有线程或独立分配器。
 */
#include <bbt/coroutine/detail/StackPool.hpp>
#include <algorithm>
#include <exception>
#include <limits>
#include <unistd.h>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

namespace bbt::coroutine::detail {

void StackReturner::operator()(Stack* stack) const noexcept {
    if (stack != nullptr)
        pool->Release(stack);
}

StackWaitError::StackWaitError(WaitStatus status)
    : std::runtime_error("coroutine stack acquisition interrupted"), m_status(status) {}

WaitStatus StackWaitError::Status() const noexcept {
    return m_status;
}

struct StackPool::Allocation {
    Allocation(StackPool* owner, BucketKey bucket_key)
        : pool(owner), key(bucket_key), lease(nullptr, StackReturner{owner}) {}
    ~Allocation() {
        // 请求共享保活，栈本身始终独占；最后一个 owner 兜底撤销未交付的预留。
        if (reserved) {
            std::lock_guard<std::mutex> lock(pool->m_mutex);
            pool->_RollbackLocked(key);
        }
    }
    StackPool* pool;
    BucketKey key;
    std::mutex mutex;
    bool reserved{false}, allocating{false}, cancelled{false}, done{false};
    StackLease lease;
    std::exception_ptr error;
    sync::CoWaiter::SPtr waiter;
    Deadline deadline{Deadline::max()};
};

namespace {
StackRequest NormalizeRequest(StackRequest request) {
    request.usable_bytes = Stack::NormalizeSize(request.usable_bytes);
    if (request.protect && request.usable_bytes >
                               static_cast<size_t>(std::numeric_limits<std::ptrdiff_t>::max()) - getpagesize())
        throw std::length_error("coroutine stack guard page size overflow");
    return request;
}
} // namespace

StackConfig StackPool::ValidateConfig(const StackConfig& config, StackRequest default_stack) {
    auto result = config;
    default_stack = NormalizeRequest(default_stack);
    if (result.policy == StackPolicy::Strict) {
        if (result.strict_classes.empty() || result.auto_max_objects || result.auto_max_usable_bytes)
            throw std::invalid_argument("invalid Strict stack budget");
        size_t objects = 0, bytes = 0;
        bool found_default = false;
        std::map<BucketKey, size_t> keys;
        for (auto& limit : result.strict_classes) {
            limit.request = NormalizeRequest(limit.request);
            const BucketKey key{limit.request.usable_bytes, limit.request.protect};
            const size_t count = limit.max_objects;
            const size_t size = key.first;
            if (!count || !keys.emplace(key, count).second)
                throw std::invalid_argument("duplicate or empty Strict stack class");
            if (count > std::numeric_limits<size_t>::max() / size ||
                objects > std::numeric_limits<size_t>::max() - count ||
                bytes > std::numeric_limits<size_t>::max() - count * size)
                throw std::length_error("Strict stack budget overflow");
            objects += count;
            bytes += count * size;
            found_default |= key == BucketKey{default_stack.usable_bytes, default_stack.protect};
        }
        if (!found_default)
            throw std::invalid_argument("default stack class missing from Strict budget");
    } else if (result.policy == StackPolicy::Auto) {
        if (!result.strict_classes.empty() || !result.auto_max_objects ||
            !result.auto_max_usable_bytes || default_stack.usable_bytes > result.auto_max_usable_bytes)
            throw std::invalid_argument("invalid Auto stack budget");
    } else {
        throw std::invalid_argument("invalid stack policy");
    }
    return result;
}

void StackPool::Configure(const StackConfig& config, StackRequest default_stack) {
    auto validated = ValidateConfig(config, default_stack);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_borrowed_count || m_pending_count)
            throw std::logic_error("stack policy cannot change with outstanding stacks");
    }
    // 启动前旧预热缓存不占新预算；先完成值校验，最后以 noexcept move 提交策略。
    ReleaseUnused();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_borrowed_count || m_pending_count)
        throw std::logic_error("stack policy cannot change with outstanding stacks");
    m_policy = std::move(validated);
    m_legacyLimit = false;
}

StackPoolSnapshot StackPool::Observe() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return {m_live_count, m_borrowed_count, m_live_count - m_borrowed_count,
            m_pending_count, m_committedBytes, m_pendingBytes};
}

bool StackPool::CanAdmit(StackRequest request, size_t count) const {
    request.usable_bytes = Stack::NormalizeSize(request.usable_bytes);
    if (count == 0)
        return false;
    const BucketKey key{request.usable_bytes, request.protect};
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t held_objects = 0, held_pending = 0;
    const auto bucket = m_buckets.find(key);
    if (bucket != m_buckets.end()) {
        held_objects = bucket->second->live;
        held_pending = bucket->second->pending;
    }
    if (m_policy.policy == StackPolicy::Strict) {
        const auto declared = std::find_if(m_policy.strict_classes.begin(), m_policy.strict_classes.end(),
                                           [&key](const StackClassLimit& limit) {
                                               return BucketKey{limit.request.usable_bytes, limit.request.protect} == key;
                                           });
        if (declared == m_policy.strict_classes.end())
            return false;
        const size_t used = held_objects + held_pending;
        return used < declared->max_objects && declared->max_objects - used >= count;
    }
    // Auto 与兼容默认入口共用真实预算口径；只读，不回收 idle、不改计数。
    const size_t objects = m_legacyLimit ? g_bbt_coroutine_config->RuntimeSnapshot().max_coroutine
                                         : m_policy.auto_max_objects;
    const size_t used = (m_legacyLimit ? m_borrowed_count : m_live_count) + m_pending_count;
    if (used >= objects || objects - used < count)
        return false;
    const size_t free_bytes = m_policy.auto_max_usable_bytes - m_committedBytes - m_pendingBytes;
    return free_bytes / key.first >= count;
}

bool StackPool::_FitsLocked(const BucketKey& key, Bucket* bucket) const {
    if (m_policy.policy == StackPolicy::Strict) {
        for (const auto& limit : m_policy.strict_classes)
            if (BucketKey{limit.request.usable_bytes, limit.request.protect} == key)
                return bucket->live + bucket->pending < limit.max_objects;
        return false;
    }
    const size_t objects = m_legacyLimit ? g_bbt_coroutine_config->RuntimeSnapshot().max_coroutine : m_policy.auto_max_objects;
    const size_t used = (m_legacyLimit ? m_borrowed_count : m_live_count) + m_pending_count;
    return used < objects &&
           key.first <= m_policy.auto_max_usable_bytes - m_committedBytes - m_pendingBytes;
}

std::shared_ptr<StackPool::Allocation> StackPool::_Prepare(StackRequest request) {
    request = NormalizeRequest(request);
    const BucketKey key{request.usable_bytes, request.protect};
    auto allocation = std::make_shared<Allocation>(this, key);
    for (;;) {
        Stack* discard = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_legacyLimit && m_borrowed_count + m_pending_count >=
                                     g_bbt_coroutine_config->RuntimeSnapshot().max_coroutine)
                return nullptr;
            if (m_policy.policy == StackPolicy::Strict) {
                const auto declared = std::find_if(m_policy.strict_classes.begin(), m_policy.strict_classes.end(),
                                                   [&key](const StackClassLimit& limit) {
                                                       return BucketKey{limit.request.usable_bytes, limit.request.protect} == key;
                                                   });
                if (declared == m_policy.strict_classes.end())
                    return nullptr;
            }
            auto* bucket = _GetBucketLocked(key);
            Stack* item = nullptr;
            if (bucket->Pop(item)) {
                --bucket->idle;
                ++bucket->borrowed;
                ++m_borrowed_count;
                allocation->lease.reset(item);
                allocation->done = true;
                return allocation;
            }
            if (_FitsLocked(key, bucket)) {
                ++bucket->pending;
                ++m_pending_count;
                m_pendingBytes += key.first;
                allocation->reserved = true;
                return allocation;
            }
            _EraseBucketIfEmptyLocked(key, bucket);
            // Auto 只有预算压力时逐个摘 idle；精确桶永远不向大桶借栈。
            if (m_policy.policy == StackPolicy::Auto)
                discard = _TakeIdleLocked();
        }
        if (discard == nullptr)
            return nullptr;
        _FreeItem(discard);
    }
}

void StackPool::_RollbackLocked(const BucketKey& key) {
    auto* bucket = m_buckets.at(key).get();
    --bucket->pending;
    --m_pending_count;
    m_pendingBytes -= key.first;
    _EraseBucketIfEmptyLocked(key, bucket);
}

void StackPool::_Allocate(const std::shared_ptr<Allocation>& allocation) noexcept {
    {
        std::lock_guard<std::mutex> lock(allocation->mutex);
        if (allocation->cancelled)
            return;
        allocation->allocating = true;
    }
    std::unique_ptr<Stack> created;
    std::exception_ptr error;
    try {
        if (allocation->deadline <= std::chrono::steady_clock::now())
            throw StackWaitError(WaitStatus::TimedOut);
#ifdef BBT_COROUTINE_STACK_TESTING
        if (auto hook = s_test_before_allocation.exchange(nullptr))
            hook();
#endif
        created.reset(_AllocItem(allocation->key.first, allocation->key.second));
        if (allocation->deadline <= std::chrono::steady_clock::now())
            throw StackWaitError(WaitStatus::TimedOut);
    } catch (...) {
        error = std::current_exception();
    }
    // 取消与提交由请求锁仲裁；memalign/mprotect 不持请求锁或池锁。
    {
        std::lock_guard<std::mutex> request_lock(allocation->mutex);
        {
            std::lock_guard<std::mutex> pool_lock(m_mutex);
            auto* bucket = m_buckets.at(allocation->key).get();
            if (created && !error && !allocation->cancelled) {
                --bucket->pending;
                --m_pending_count;
                m_pendingBytes -= allocation->key.first;
                ++bucket->live;
                ++bucket->borrowed;
                ++m_live_count;
                ++m_borrowed_count;
                m_committedBytes += allocation->key.first;
                allocation->lease.reset(created.release());
            } else {
                _RollbackLocked(allocation->key);
            }
            allocation->reserved = false;
        }
        allocation->error = error;
        allocation->done = true;
        allocation->allocating = false;
    }
    if (created)
        _FreeItem(created.release());
#ifdef BBT_COROUTINE_STACK_TESTING
    if (auto hook = s_test_after_commit.exchange(nullptr))
        hook();
#endif
    if (allocation->waiter)
        allocation->waiter->Notify();
}

void StackPool::_Cancel(const std::shared_ptr<Allocation>& allocation) noexcept {
    StackLease returned(nullptr, StackReturner{this});
    {
        std::lock_guard<std::mutex> request_lock(allocation->mutex);
        allocation->cancelled = true;
        returned = std::move(allocation->lease);
        if (allocation->reserved && !allocation->allocating) {
            std::lock_guard<std::mutex> pool_lock(m_mutex);
            m_allocations.erase(std::remove(m_allocations.begin(), m_allocations.end(), allocation), m_allocations.end());
            _RollbackLocked(allocation->key);
            allocation->reserved = false;
        }
    }
}

StackLease StackPool::Acquire(StackRequest request, const WaitOptions& wait) {
    if (wait.deadline <= std::chrono::steady_clock::now())
        throw StackWaitError(WaitStatus::TimedOut);
    const auto processer = Processer::TryGetLocalProcesser();
    if (processer && processer->GetCurrentCoroutine() &&
        processer->GetCurrentCoroutine()->IsCancelRequested())
        throw StackWaitError(WaitStatus::Cancelled);
    auto allocation = _Prepare(request);
    if (!allocation)
        return StackLease(nullptr, StackReturner{this});
    allocation->deadline = wait.deadline;
    if (allocation->done)
        return std::move(allocation->lease);
    if (!processer || !processer->GetCurrentCoroutine()) {
        _Allocate(allocation);
    } else {
        allocation->waiter = sync::CoWaiter::Create();
        WaitStatus status;
        try {
            status = allocation->waiter->WaitWithCallback(wait, [this, allocation] {
                try {
#ifdef BBT_COROUTINE_STACK_TESTING
                    if (s_test_fail_post.exchange(false))
                        throw std::bad_alloc();
#endif
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_allocations.push_back(allocation);
                } catch (...) {
                    {
                        std::lock_guard<std::mutex> lock(allocation->mutex);
                        allocation->error = std::current_exception();
                        allocation->done = true;
                    }
                    _Cancel(allocation);
                    allocation->waiter->Notify();
                }
                return true;
            });
        } catch (...) {
            _Cancel(allocation);
            throw;
        }
        if (status != WaitStatus::Completed) {
            _Cancel(allocation);
            throw StackWaitError(status);
        }
    }
    std::lock_guard<std::mutex> lock(allocation->mutex);
    if (allocation->error)
        std::rethrow_exception(allocation->error);
    return std::move(allocation->lease);
}

void StackPool::DrainAllocations() {
    // 每拍一个，避免持续提交让维护流无限排空；不另建线程或通用任务平台。
    std::shared_ptr<Allocation> allocation;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_allocations.empty())
            return;
        allocation = std::move(m_allocations.front());
        m_allocations.erase(m_allocations.begin());
    }
    _Allocate(allocation);
}

#ifdef BBT_COROUTINE_STACK_TESTING
std::atomic_bool StackPool::s_test_fail_post{false};
std::atomic<void (*)()> StackPool::s_test_before_allocation{nullptr};
std::atomic<void (*)()> StackPool::s_test_after_commit{nullptr};
void StackPool::_TestFailNextPost() noexcept {
    s_test_fail_post.store(true);
}
void StackPool::_TestBeforeAllocation(void (*hook)()) noexcept {
    s_test_before_allocation.store(hook);
}
void StackPool::_TestAfterCommit(void (*hook)()) noexcept {
    s_test_after_commit.store(hook);
}
#endif

} // namespace bbt::coroutine::detail
