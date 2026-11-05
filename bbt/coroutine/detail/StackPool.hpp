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

namespace bbt::coroutine::detail {

/* 栈池：按 (U, protect) 精确复用固定栈，减少频繁分配与系统调用，不从更大桶借、不归档到二次幂。
 * 计数口径（live = idle + borrowed，pending = 已预留容量但分配未完成）全由一把池级 mutex 保护，
 * 容量上限约束的是跨桶的「借出 + 已预留」，不是每桶上限、也不是内存字节预算。
 * 单池锁是已知吞吐上限：只有聚焦回归证明它成为阻断瓶颈时才升级为分桶同步，不提前做。 */
class StackPool {
public:
    typedef std::unique_ptr<StackPool> UPtr;
    typedef Stack ItemType;

    /* 进程内唯一实例，寿命与进程一致：持有者刻意泄漏，不参与静态析构，免得仍在使用栈池的 worker 撞上已销毁的池。 */
    static UPtr& GetInstance();

    StackPool();
    ~StackPool();
    /* 定时更新：S 线程（或 LoopOnce 调用线程）在每轮 Scheduler::_OnUpdate 推进，采样借用数并回收超出全池目标的空闲栈。 */
    void OnUpdate();

    /* 归还 Stack；按 Stack 自身 U/保护属性回桶，不读全局默认值。归还后对象归池，调用方不得再访问；
     * 必须归还本池借出的对象且恰好一次；防御分支仅在桶不存在或桶借出计数为 0 时锁外销毁。 */
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

    /* 池管理的 Stack 对象总数（= idle + borrowed，尚未建完的 pending 不计），锁内精确计数 */
    size_t AllocSize();
    /* 借用数的指数滑动平均：每个采样间隔按 7/8 旧值 + 1/8 当前(borrowed+pending)
     * 更新，首次采样前取当前值；OnUpdate 用它算全池保留目标 */
    size_t GetCoAvgCount();
    /* 当前借出 + 申请中的 Co 数量；不用队列近似 Size 推导 */
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
    /* 单个尺寸/属性桶；pool 生命周期由 Bucket 自己兜底排空（含构造中途抛出） */
    struct Bucket {
        explicit Bucket(size_t capacity)
        {
            pool.reserve(capacity);
        }
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
                return false; // 沿用归还失败分支：扣 live 后锁外释放栈
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

    static constexpr size_t kBucketInitialCapacity{1024};

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
