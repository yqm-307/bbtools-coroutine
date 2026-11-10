#pragma once
#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/Stack.hpp>
#include <bbt/coroutine/runtime/RuntimeOptions.hpp>
#include <bbt/coroutine/sync/WaitTypes.hpp>

namespace bbt::coroutine::detail
{

class StackPool;

/** @brief 栈的唯一归还责任；pool 必须活过全部 lease。 */
struct StackReturner {
    StackPool* pool{nullptr};
    void operator()(Stack* stack) const noexcept;
};
using StackLease = std::unique_ptr<Stack, StackReturner>;

/** @brief 栈等待失败；保留取消、超时与基础设施失败的区别。 */
class StackWaitError : public std::runtime_error {
public:
    explicit StackWaitError(WaitStatus status);
    WaitStatus Status() const noexcept;

private:
    WaitStatus m_status;
};

/** @brief 同一把池锁下的精确对象数与可用字节快照，不含保护页。 */
struct StackPoolSnapshot {
    size_t live{0}, borrowed{0}, idle{0}, pending{0};
    size_t committed_usable_bytes{0}, pending_usable_bytes{0};
};

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
 * 显式 Strict 限制每类 live+pending；Auto 限制全池 live+pending 与可用字节。
 * 兼容默认入口另保留旧的跨桶 borrowed+pending 上限。
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

    /** @brief 获取精确规格；Co 缓存未命中在既有 native 维护 lane 分配，无同步兜底。
     * 等待使用调用方 deadline 和协程取消。容量拒绝返回空 lease；其它失败抛出，不发布任务。
     */
    StackLease Acquire(StackRequest request, const WaitOptions& wait = {});

    /** @brief 启动前完整校验并一次应用；有 borrowed/pending 时拒绝，旧 idle 缓存先回收。 */
    void Configure(const StackConfig& config, StackRequest default_stack);
    static StackConfig ValidateConfig(const StackConfig& config, StackRequest default_stack);

    /**
     * @brief 创建前只读准入查询：按当前策略再接纳 count 个该精确规格的栈是否仍在预算内。
     *
     * 只读快照，不预留、不改变计数；与真实申请并发时结果仍可能失效，最终以
     * Acquire 的交付/拒绝为准。用于一次性启动多个 worker 前先拒绝明显不可行的规格。
     */
    bool CanAdmit(StackRequest request, size_t count) const;
    StackPoolSnapshot Observe() const;

    /** @brief 现有 native 维护执行流推进有界在途栈分配；业务 worker 禁止调用。 */
    void DrainAllocations();

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
    static void _TestFailNextPost() noexcept;
    static void _TestBeforeAllocation(void (*hook)()) noexcept;
    static void _TestAfterCommit(void (*hook)()) noexcept;
#endif
protected:
    /* 真正创建 min(预热数, 全池上限) 个独立默认栈；构造失败会清理已建对象后抛出，
     * 故不标 noexcept。 */
    size_t _Preload(size_t preload_size);
    ItemType* _AllocItem(size_t stack_size, bool stack_protect);
    static void _FreeItem(ItemType* item);
private:
    /* 单个尺寸/属性桶；pool 生命周期由 Bucket 自己兜底排空（含构造中途抛出） */
    struct Bucket
    {
        explicit Bucket(size_t capacity) { pool.reserve(capacity); }
        ~Bucket()
        {
            for (auto* item : pool)
                StackPool::_FreeItem(item);
        }
        /* 发布后的桶访问均由池级 m_mutex 串行化，无需再维护并发队列和原子计数。
         * LIFO 优先复用刚归还的栈；capacity 只是预留，不是缓存数量上限。 */
        bool Push(ItemType* item) noexcept
        {
            try {
                pool.push_back(item);
                return true;
            } catch (...) {
                return false;   // 沿用归还失败分支：扣 live 后锁外释放栈
            }
        }
        bool Pop(ItemType*& item) noexcept
        {
            if (pool.empty())
                return false;
            item = pool.back();
            pool.pop_back();
            return true;
        }
        Bucket(const Bucket&) = delete;
        Bucket& operator=(const Bucket&) = delete;

        std::vector<ItemType*> pool;
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
    struct Allocation;
    std::shared_ptr<Allocation> _Prepare(StackRequest request);
    void _Allocate(const std::shared_ptr<Allocation>& allocation) noexcept;
    void _Cancel(const std::shared_ptr<Allocation>& allocation) noexcept;
    void _RollbackLocked(const BucketKey& key);
    bool _FitsLocked(const BucketKey& key, Bucket* bucket) const;

    static constexpr size_t kBucketInitialCapacity{1024};

    std::map<BucketKey, std::unique_ptr<Bucket>> m_buckets;
    mutable std::mutex m_mutex;
    size_t m_live_count{0};
    size_t m_borrowed_count{0};
    size_t m_pending_count{0};
    size_t m_committedBytes{0};
    size_t m_pendingBytes{0};
    StackConfig m_policy{StackPolicy::Auto, {}, 0, 256 * 1024 * 1024};
    bool m_legacyLimit{true};
    std::vector<std::shared_ptr<Allocation>> m_allocations;
    size_t m_co_avg{0};
    const float m_rate{0};
    bbt::core::clock::Timestamp<> m_prev_adjust_pool_ts;
    bbt::core::clock::Timestamp<> m_prev_rtts_sample_ts;
#ifdef BBT_COROUTINE_STACK_TESTING
    static std::atomic_bool s_test_fail_push;
    static std::atomic_bool s_test_fail_post;
    static std::atomic<void (*)()> s_test_before_allocation;
    static std::atomic<void (*)()> s_test_after_commit;
#endif
};

}
