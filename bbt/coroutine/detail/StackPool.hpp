#pragma once
#include <atomic>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <bbt/core/thread/sync/Queue.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/Stack.hpp>

namespace bbt::coroutine::detail
{

/**
 * @brief 栈池：按「页对齐可用字节数 U + 保护属性」精确复用固定栈。
 *
 * 使用固定栈 + 栈池复用是为了减少频繁内存分配和系统调用；多尺寸共存时
 * 分桶键就是 (U, protect)，不从更大桶借、不向二次幂归档。
 *
 * 计数口径（全部由一把池级 mutex 保护，避免多个 atomic 字段假装事务）：
 *   live     = 池管理的已分配栈对象数（= idle + borrowed，稳定时成立）
 *   idle     = 缓存中可复用的空闲对象数
 *   borrowed = 已借出给调用方的对象数
 *   pending  = 已预留容量但分配尚未完成的对象数
 * 容量上限约束的是「跨桶的同时借出 + 已预留」，不是每桶上限、也不是内存字节预算。
 *
 * ponytail: 当前用一把池级 mutex 串行化桶元数据，吞吐上限是并发申请/归还
 * 共享同一临界区；只有聚焦回归证明它成为阻断瓶颈时，才升级为分桶同步并
 * 保留独立的全池容量协议。不提前做该升级。
 */
class StackPool
{
public:
    typedef std::unique_ptr<StackPool> UPtr;
    typedef Stack ItemType;

    static UPtr& GetInstance();

    StackPool();
    ~StackPool();
    /* 定时更新：采样 + 按全池目标回收空闲栈 */
    void OnUpdate();

    /* 归还Stack；按 Stack 自身 U/保护属性回桶，不读全局默认值 */
    void Release(ItemType* item);
    /**
     * @brief 回收所有桶中的空闲栈，返回实际释放数。
     *
     * 借出和申请中的栈不受影响，可与申请/归还并发调用；不停止运行时。
     * 要据此检查完全清空，调用方须先停止提交并确认所有借出栈已归还。
     */
    size_t ReleaseUnused();
    /* 申请Stack：默认配置兼容入口（转调显式重载） */
    ItemType* Apply();
    /* 申请Stack：显式字节数 + 保护属性；容量不足返回 nullptr */
    ItemType* Apply(size_t stack_size, bool stack_protect);

    /* 已经创建的Stack总数（池管理对象数），锁内精确计数 */
    size_t AllocSize();
    /* 最近使用Co的平均值 */
    size_t GetCoAvgCount();
    /* 当前借出 + 申请中的Co数量；不用队列近似 Size 推导 */
    size_t GetCurCoNum();

#ifdef BBT_COROUTINE_STACK_TESTING
    /* 窄测试 seam：让下一次缓存入队（Push）穿过真实结果检查分支失败，
     * 一次性消费。仅 BBT_COROUTINE_STACK_TESTING 构建存在。 */
    static void _TestFailNextPoolPush() noexcept;
#endif
protected:
    /* 真正创建 min(预热数, 全池上限) 个独立默认栈；构造失败会清理已建对象后抛出，
     * 故不标 noexcept。 */
    size_t _Preload(size_t preload_size);
    ItemType* _AllocItem(size_t stack_size, bool stack_protect);
    static void _FreeItem(ItemType* item);
private:
#ifdef BBT_COROUTINE_VALGRIND
    /* boost::lockfree::queue 的 freelist 使用 tagged pointer，Valgrind 无法从进程级
     * 单例追踪其节点；诊断构建改用同语义的可追踪容器。生产构建仍保留 lock-free 队列。 */
    class ValgrindPool
    {
    public:
        /* 参数仅用于与生产 Queue 的构造签名对齐；诊断容器按需增长。 */
        explicit ValgrindPool(size_t) {}

        bool Push(ItemType* item) noexcept;
        bool Pop(ItemType*& item) noexcept;
        size_t Size() const noexcept;

    private:
        mutable std::mutex  m_mutex;
        std::deque<ItemType*> m_items;
    };
    using PoolType = ValgrindPool;
#else
    using PoolType = bbt::core::thread::Queue<ItemType*>;
#endif

    /* 单个尺寸/属性桶；pool 生命周期由 Bucket 自己兜底排空（含构造中途抛出） */
    struct Bucket
    {
        explicit Bucket(size_t capacity): pool(capacity) {}
        ~Bucket()
        {
            ItemType* item = nullptr;
            while (pool.Pop(item))
                StackPool::_FreeItem(item);
        }
        Bucket(const Bucket&) = delete;
        Bucket& operator=(const Bucket&) = delete;

        PoolType pool;
        size_t live{0};
        size_t idle{0};
        size_t borrowed{0};
        size_t pending{0};
    };
    using BucketKey = std::pair<size_t, bool>;

    /* 以下 *_Locked 必须在 m_mutex 持有期调用 */
    Bucket* _GetBucketLocked(const BucketKey& key);
    void _EraseBucketIfEmptyLocked(const BucketKey& key, Bucket* bucket);
    /* 摘一个空闲对象并更新计数；调用方须持锁，成功返回对象（锁外销毁） */
    ItemType* _TakeIdleLocked();

    static constexpr size_t kBucketQueueCapacity{1024};

    std::map<BucketKey, std::unique_ptr<Bucket>> m_buckets;
    mutable std::mutex m_mutex;
    size_t m_live_count{0};
    size_t m_borrowed_count{0};
    size_t m_pending_count{0};
    size_t m_co_avg{0};
    const float m_rate{0};
    bbt::core::clock::Timestamp<> m_prev_adjust_pool_ts;
    bbt::core::clock::Timestamp<> m_prev_rtts_sample_ts;
#ifdef BBT_COROUTINE_STACK_TESTING
    static std::atomic_bool s_test_fail_push;
#endif
};

}
