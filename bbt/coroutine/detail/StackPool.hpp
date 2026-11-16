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
#include <bbt/coroutine/detail/StackBucket.hpp>
#include <bbt/coroutine/detail/StackPoolTypes.hpp>
#include <bbt/coroutine/runtime/RuntimeOptions.hpp>
#include <bbt/coroutine/sync/WaitTypes.hpp>

namespace bbt::coroutine::detail {

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
class StackPool {
public:
    typedef std::unique_ptr<StackPool> UPtr;
    typedef Stack ItemType;

    /**
     * @brief 进程内唯一实例，寿命与进程一致：持有者刻意泄漏，不参与静态析构，
     * 免得仍在使用栈池的 worker 撞上已销毁的池。
     */
    static UPtr& GetInstance();

    StackPool();
    ~StackPool();

    /**
     * @brief 启动前完整校验并一次应用；有 borrowed/pending 时拒绝，旧 idle 缓存先回收。
     */
    void Configure(const StackConfig& config, StackRequest default_stack);
    static StackConfig ValidateConfig(const StackConfig& config, StackRequest default_stack);

    /**
     * @brief 获取精确规格；Co 缓存未命中在既有 native 维护 lane 分配，无同步兜底。
     *
     * 等待使用调用方 deadline 和协程取消。容量拒绝返回空 lease；其它失败抛出，不发布任务。
     */
    StackLease Acquire(StackRequest request, const WaitOptions& wait = {});

    /**
     * @brief 申请 Stack：无参版本按默认配置转调显式重载；显式版本按字节数 + 保护属性申请，
     * 容量不足返回 nullptr。
     */
    ItemType* Apply();
    ItemType* Apply(size_t stack_size, bool stack_protect);

    /**
     * @brief 归还 Stack；按 Stack 自身 U/保护属性回桶，不读全局默认值。
     *
     * 归还后对象归池，调用方不得再访问；必须归还本池借出的对象且恰好一次；
     * 防御分支仅在桶不存在或桶借出计数为 0 时锁外销毁。
     */
    void Release(ItemType* item);

    /**
     * @brief 创建前只读准入查询：按当前策略再接纳 count 个该精确规格的栈是否仍在预算内。
     *
     * 只读快照，不预留、不改变计数；与真实申请并发时结果仍可能失效，最终以
     * Acquire 的交付/拒绝为准。用于一次性启动多个 worker 前先拒绝明显不可行的规格。
     */
    bool CanAdmit(StackRequest request, size_t count) const;

    /**
     * @brief 在同一把池锁下读取对象数与可用字节的精确快照。
     */
    StackPoolSnapshot Observe() const;

    /**
     * @brief 定时更新：S 线程（或 LoopOnce 调用线程）在每轮 Scheduler::_OnUpdate 推进，
     * 采样借用数并回收超出全池目标的空闲栈。
     */
    void OnUpdate();

    /**
     * @brief 现有 native 维护执行流推进有界在途栈分配；业务 worker 禁止调用。
     */
    void DrainAllocations();

    /**
     * @brief 回收所有桶中的空闲栈，返回实际释放数。
     *
     * 借出和申请中的栈不受影响，可与申请/归还并发调用；不停止运行时。
     * 要据此检查完全清空，调用方须先停止提交并确认所有借出栈已归还。
     */
    size_t ReleaseUnused();

    /**
     * @brief 池管理的 Stack 对象总数（= idle + borrowed，尚未建完的 pending 不计），锁内精确计数。
     */
    size_t AllocSize();

    /**
     * @brief 借用数的指数滑动平均：每个采样间隔按 7/8 旧值 + 1/8 当前(borrowed+pending)
     * 更新，首次采样前取当前值；OnUpdate 用它算全池保留目标。
     */
    size_t GetCoAvgCount();

    /**
     * @brief 当前借出 + 申请中的 Co 数量；不用队列近似 Size 推导。
     */
    size_t GetCurCoNum();

#ifdef BBT_COROUTINE_STACK_TESTING

    /**
     * @brief 窄测试 seam：让下一次缓存入队（Push）、维护流投递失败，或在分配前/提交后
     * 插入钩子；均一次性消费，仅 BBT_COROUTINE_STACK_TESTING 构建存在。
     */
    static void _TestFailNextPoolPush() noexcept;
    static void _TestFailNextPost() noexcept;
    static void _TestBeforeAllocation(void (*hook)()) noexcept;
    static void _TestAfterCommit(void (*hook)()) noexcept;
#endif

private:
    /**
     * 桶析构要销毁缓存中的空闲栈，复用同一个带 Profiler 事件的销毁入口。
     */
    friend struct StackBucket;

    struct Allocation;
    using BucketKey = std::pair<size_t, bool>;

    /**
     * 真正创建 min(预热数, 全池上限) 个独立默认栈；构造失败会清理已建对象后抛出，
     * 故不标 noexcept。
     */
    size_t _Preload(size_t preload_size);

    ItemType* _AllocItem(size_t stack_size, bool stack_protect);
    static void _FreeItem(ItemType* item);

    /**
     * 以下 *_Locked 必须在 m_mutex 持有期调用。
     */
    StackBucket* _GetBucketLocked(const BucketKey& key);
    void _EraseBucketIfEmptyLocked(const BucketKey& key, StackBucket* bucket);
    void _RollbackLocked(const BucketKey& key);
    bool _FitsLocked(const BucketKey& key, StackBucket* bucket) const;

    /**
     * 摘一个空闲对象并更新计数；调用方须持锁，成功返回对象（锁外销毁）。
     */
    ItemType* _TakeIdleLocked();

    std::shared_ptr<Allocation> _Prepare(StackRequest request);
    void _Allocate(const std::shared_ptr<Allocation>& allocation) noexcept;
    void _Cancel(const std::shared_ptr<Allocation>& allocation) noexcept;

private:
    static constexpr size_t kBucketInitialCapacity{1024};

    std::map<BucketKey, std::unique_ptr<StackBucket>> m_buckets;
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
