#pragma once
#include <boost/noncopyable.hpp>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/utils/lockfree/concurrentqueue.h>

#include <atomic>
#include <exception>
#include <functional>
#include <future>

namespace bbt::coroutine::pool {

/** @file CoPool.hpp @brief 固定并发度的协程池：任务经队列投递给池内协程，异常在池内隔离后交付。 */

/**
 * @brief 协程池，固定数量的协程处理任务，数量不可动态增加（池不创建线程，执行体都是协程）；
 *        池内任务执行阻塞操作会占住该协程。
 *
 * 异常语义：
 * - worker 协程执行任务抛出的异常被池捕获，不传播到协程运行时，池继续工作；
 * - 未设置回调时异常被记录（GetUnhandledExceptionCount），设置回调后交付回调；
 * - SubmitWithFuture 提交的任务异常通过返回的 std::future 交付；
 * - 异常回调自身抛出异常同样被隔离（记录后继续）。
 */
class CoPool : boost::noncopyable {
public:
    /**
     * @brief 创建池并注册 max_co 个 work 协程 + 1 个 monitor；须在 runtime 初始化后调用，max_co 须 > 0（断言）。
     * @return 池的共享 owner；无可用 worker 抛 std::runtime_error，栈申请失败抛 length_error/bad_alloc。
     */
    static std::shared_ptr<CoPool> Create(int max_co);

    /// @brief 直接构造并注册池内协程，通常走 Create；co_max 必须 > 0（断言）
    BBTATTR_FUNC_CTOR_HIDDEN
    explicit CoPool(int co_max);
    /// @brief 析构：丢弃队列中滞留任务，再 Release 等池内协程退出
    virtual ~CoPool();

    /**
     * @brief 投递任务，由池内空闲协程执行；只入队，不阻塞调用方。
     * @return 0 入队成功；-1 池已停止、分配失败或入队失败，任务不会被执行
     */
    virtual int Submit(const CoPoolWorkCallback& workfunc);
    /// @brief 同上，接受右值回调（可移入捕获的 lambda）
    virtual int Submit(CoPoolWorkCallback&& workfunc);

    /**
     * @brief 提交任务并返回携带结果的 future
     *        任务异常通过 future.get() 抛出，正常完成则 get() 返回。
     * @return 空 future 表示提交失败（池已停止或内存不足）
     */
    virtual std::future<void> SubmitWithFuture(const CoPoolWorkCallback& workfunc);
    /// @brief 同上，接受右值回调
    virtual std::future<void> SubmitWithFuture(CoPoolWorkCallback&& workfunc);

    /**
     * @brief 设置任务异常回调（不再仅计数），传 nullptr 恢复默认；回调在捕获异常的 work 协程里就地执行，
     *        阻塞多久就占住该协程多久，取用时锁内拷贝到局部、锁外调用。
     */
    virtual void SetExceptionCallback(std::function<void(std::exception_ptr)> callback);

    /**
     * @brief 未被回调/future 处理的异常数量；只增不减（回调自身抛出也计一次），任意线程可读。
     */
    virtual uint64_t GetUnhandledExceptionCount() const noexcept;

    /**
     * @brief 停止接收新任务，等池内协程跑完当前任务后退出，队列滞留任务取消式排空（带 future 的以
     *        broken_promise 兑现）；幂等。不要在池内任务里调用它等自己。
     */
    virtual void Release();

protected:
    /* 注册 m_max_co_num 个 work 协程和一个 monitor 协程；只在构造时调用一次 */
    void _Start();
    /* 工作协程主循环：取任务、执行、异常隔离；退出循环后递减 m_running_co_num */
    void _WorkCo();
    /* 监控协程主循环：每 1ms 检查队列并补唤醒漏掉通知的 work 协程 */
    void _MonitorCo();

private:
    /* 处理单个任务的异常（统一隔离入口）：future 路径优先，其次回调，最后只计数 */
    void _HandleWorkException(class Work* work, const std::exception_ptr& eptr) noexcept;

    /* Release 置 false 后，work/monitor 协程在下一轮循环条件处退出 */
    std::atomic_bool m_is_running{true};
    const int m_max_co_num{0};
    /* Work* 的所有权在这里：入队即交给池，由取到它的 work 协程 delete */
    moodycamel::ConcurrentQueue<Work*> m_works_queue;
    /* 仍在主循环里的协程数（含 monitor）；Release 轮询它归零，再等 latch */
    std::atomic_int m_running_co_num{0};

    /* work 协程无任务时的等待点；入队方 NotifyOne 唤醒一个 */
    std::shared_ptr<sync::CoCond> m_cond{nullptr};
    std::mutex m_cond_mtx;

    /* 保护 m_exception_callback。取用时在锁内拷贝到局部、调用放在锁外：回调里若再调
       SetExceptionCallback 会拿同一把非递归 mutex，持锁调用就自锁 */
    std::mutex m_exception_cb_mtx;
    std::function<void(std::exception_ptr)> m_exception_callback{nullptr};

    /* 只由 fetch_add 递增，供 GetUnhandledExceptionCount 读取 */
    std::atomic_uint64_t m_unhandled_exception_count{0};

    /* 计数 = work 协程数 + 1（monitor 也占一格）；每个池内协程退出前 Down 一次 */
    bbt::core::thread::CountDownLatch m_latch;
};

}; // namespace bbt::coroutine::pool
