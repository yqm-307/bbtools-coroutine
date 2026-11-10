#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/StackPool.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>

namespace bbt::coroutine::detail
{

#ifdef BBT_COROUTINE_STACK_TESTING
std::atomic_bool StackPool::s_test_fail_push{false};

void StackPool::_TestFailNextPoolPush() noexcept
{
    s_test_fail_push.store(true, std::memory_order_release);
}
#endif

StackPool::UPtr& StackPool::GetInstance()
{
    /* 进程寿命：运行期对象，不在静态退出期析构 */
    static UPtr* _holder = new UPtr{new StackPool()};

    return *_holder;
}

StackPool::StackPool():
    m_prev_adjust_pool_ts(bbt::core::clock::now<>())
{
    /* RuntimeMain may configure the pool before publishing the immutable snapshot. */
    m_policy.auto_max_objects = g_bbt_coroutine_config->m_cfg_max_coroutine;
    if (g_bbt_coroutine_config->m_cfg_static_coroutine > 0)
        _Preload(g_bbt_coroutine_config->m_cfg_static_coroutine);

}

/* 每个 Bucket 析构时排空自身缓存（含 _Preload 中途抛出、StackPool 构造未完成的
 * 展开路径）。前置条件：本池没有未归还的借出栈——借出对象不在缓存里，不在此销毁。 */
StackPool::~StackPool() = default;

StackPool::Bucket* StackPool::_GetBucketLocked(const BucketKey& key)
{
    auto it = m_buckets.find(key);
    if (it != m_buckets.end())
        return it->second.get();

    auto inserted = m_buckets.emplace(
        key, std::unique_ptr<Bucket>(new Bucket(kBucketInitialCapacity)));
    return inserted.first->second.get();
}

void StackPool::_EraseBucketIfEmptyLocked(const BucketKey& key, Bucket* bucket)
{
    /* 空桶目录仅在无缓存/借出/预留时可清除；任何 pin 都保持桶存活 */
    if (bucket->idle == 0 && bucket->borrowed == 0 && bucket->pending == 0)
        m_buckets.erase(key);
}

StackPool::ItemType* StackPool::_TakeIdleLocked()
{
    for (auto it = m_buckets.begin(); it != m_buckets.end(); ++it)
    {
        Bucket* bucket = it->second.get();
        if (bucket->idle == 0)
            continue;

        ItemType* item = nullptr;
        if (!bucket->Pop(item))
        {
            /* 计数与队列不一致（正常路径不该发生）：以队列为准，避免同一条目反复扫描 */
            bucket->idle = 0;
            continue;
        }

        bucket->idle--;
        bucket->live--;
        m_live_count--;
        m_committedBytes -= item->UseableSize();

        /* 先拷贝键：erase 会使 it->first 失效 */
        const BucketKey key = it->first;
        _EraseBucketIfEmptyLocked(key, bucket);
        return item;
    }

    return nullptr;
}

StackPool::ItemType* StackPool::Apply()
{
    const size_t stack_size = g_bbt_coroutine_config->RuntimeSnapshot().stack_size;
    const bool stack_protect = g_bbt_coroutine_config->RuntimeSnapshot().stack_protect;
    return Apply(stack_size, stack_protect);
}

StackPool::ItemType* StackPool::Apply(size_t stack_size, bool stack_protect)
{
    return Acquire({stack_size, stack_protect}).release();
}

void StackPool::Release(ItemType* item)
{
    if (item == nullptr)
        return;

    /* 按栈自身属性回桶：不读全局默认大小/保护开关 */
    const BucketKey key{item->UseableSize(), item->IsProtected()};
    ItemType* to_destroy = nullptr;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_buckets.find(key);
        Bucket* bucket = it == m_buckets.end() ? nullptr : it->second.get();

        bool cached = false;
        if (bucket != nullptr && bucket->borrowed > 0)
        {
#ifdef BBT_COROUTINE_STACK_TESTING
            if (s_test_fail_push.exchange(false, std::memory_order_acq_rel))
                cached = false;     /* 注入穿过真实 Push 结果检查分支 */
            else
#endif
                cached = bucket->Push(item);
        }

        if (bucket == nullptr || bucket->borrowed == 0)
        {
            /* 前置条件违反（非本池对象/重复归还）：不伪造计数，锁外销毁以免泄漏；
             * 不为重复归还/跨池非法指针增加通用所有权追踪表。 */
            to_destroy = item;
        }
        else if (cached)
        {
            bucket->borrowed--;
            m_borrowed_count--;
            bucket->idle++;
        }
        else
        {
            /* 缓存入队失败：撤回缓存、扣 live，锁外销毁；析构路径不抛异常 */
            bucket->borrowed--;
            m_borrowed_count--;
            bucket->live--;
            m_live_count--;
            m_committedBytes -= item->UseableSize();
            to_destroy = item;
        }

        if (bucket != nullptr)
            _EraseBucketIfEmptyLocked(key, bucket);
    }

    if (to_destroy != nullptr)
        _FreeItem(to_destroy);      /* 锁外 delete + Profiler 事件 */
}

size_t StackPool::ReleaseUnused()
{
    size_t released = 0;
    for (;;)
    {
        ItemType* item = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            /* 逐个在锁内摘空闲并更新计数，解锁后销毁再继续：
             * 不为回收额外分配收集 vector，避免清理路径再分配失败 */
            item = _TakeIdleLocked();
        }
        if (item == nullptr)
            break;

        _FreeItem(item);
        ++released;
    }
    return released;
}

size_t StackPool::AllocSize()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_live_count;
}

size_t StackPool::GetCurCoNum()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_borrowed_count + m_pending_count;
}

size_t StackPool::GetCoAvgCount()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_co_avg;
}

void StackPool::OnUpdate()
{
    size_t keep = 0;
    size_t to_release = 0;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_co_avg == 0)
            m_co_avg = m_borrowed_count + m_pending_count;

        if (bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(
                m_prev_rtts_sample_ts +
                bbt::core::clock::milliseconds(g_bbt_coroutine_config->RuntimeSnapshot().stackpool_sample_interval)))
        {
            m_co_avg = static_cast<size_t>(::floor(
                0.875 * static_cast<double>(m_co_avg) +
                0.125 * static_cast<double>(m_borrowed_count + m_pending_count)));
            /* 成功采样后推进采样时间，否则每个 tick 都在重采样 */
            m_prev_rtts_sample_ts = bbt::core::clock::now<>();
        }

        const size_t allowable_stack_num = static_cast<size_t>(
            ::floor((m_rate + 1) * static_cast<double>(m_co_avg)));

        if (!bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(
                m_prev_adjust_pool_ts +
                bbt::core::clock::milliseconds(g_bbt_coroutine_config->RuntimeSnapshot().stackpool_adjust_interval)))
            return;

        m_prev_adjust_pool_ts = bbt::core::clock::now<>();

        /* 全池统一保留目标 = max(最低保留数, 平均目标)；不按每桶复制最低数。
         * 只回收空闲对象，不触碰借出/申请中的栈。 */
        keep = std::max(g_bbt_coroutine_config->RuntimeSnapshot().stackpool_min_alloc_size,
                        allowable_stack_num);
        to_release = m_live_count > keep ? (m_live_count - keep) : 0;
    }

    for (size_t i = 0; i < to_release; ++i)
    {
        ItemType* idle = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_live_count <= keep)
                break;
            idle = _TakeIdleLocked();
        }
        if (idle == nullptr)
            break;      /* 无空闲可回收（借出/申请中的不回收） */

        _FreeItem(idle);    /* 锁外销毁继续 */
    }
}

StackPool::ItemType* StackPool::_AllocItem(size_t stack_size, bool stack_protect)
{
    /* 锁外大块分配（memalign + mprotect） */
    ItemType* item = new Stack(stack_size, stack_protect);
#ifdef BBT_COROUTINE_PROFILE
    /* PROFILE 只统计真实新建对象，复用不记新分配 */
    g_bbt_profiler->OnEvent_StackAlloc();
#endif
    return item;
}

void StackPool::_FreeItem(ItemType* item)
{
    delete item;
#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_StackRelease(1);
#endif
}

size_t StackPool::_Preload(size_t preload_size)
{
    // 先持有全部独立 lease，不能 Apply→Release 循环重复预热同一个对象。
    const size_t limit = std::min(preload_size, g_bbt_coroutine_config->m_cfg_max_coroutine);
    std::vector<StackLease> created;
    created.reserve(limit);
    for (size_t i = 0; i < limit; ++i) {
        auto lease = Acquire({g_bbt_coroutine_config->RuntimeSnapshot().stack_size,
                              g_bbt_coroutine_config->RuntimeSnapshot().stack_protect});
        if (!lease)
            break;
        created.push_back(std::move(lease));
    }
    const size_t count = created.size();
    created.clear();
    // 构造尚未发布：任一缓存入队失败都撤销整批预热，而不是留下半份缓存。
    if (AllocSize() != count)
        ReleaseUnused();
    return AllocSize();
}

}
