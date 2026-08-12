#pragma once
#include <boost/noncopyable.hpp>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/utils/lockfree/concurrentqueue.h>

#include <atomic>
#include <exception>
#include <functional>
#include <future>

namespace bbt::coroutine::pool
{

/**
 * @brief 协程池，池子会创建固定数量的协程来处理任务
 * 
 * 注意：
 * 1. 协程池的协程数量是固定的，不能动态增加
 * 2. 在协程池中执行任务，如过执行阻塞操作导致协程挂起，会占用这个协程
 *   导致池子中可用协程数变少
 * 3. 协程池中的协程会一直存在，直到协程池被销毁
 *
 * 异常语义（见 #189）：
 * - worker 协程执行任务抛出的异常被池捕获，不传播到协程运行时，池继续工作；
 * - 未设置回调时异常被记录（GetUnhandledExceptionCount），设置回调后交付回调；
 * - SubmitWithFuture 提交的任务异常通过返回的 std::future 交付；
 * - 异常回调自身抛出异常同样被隔离（记录后继续）。
 */
class CoPool:
    boost::noncopyable
{
public:
    static std::shared_ptr<CoPool> Create(int max_co);

    BBTATTR_FUNC_CTOR_HIDDEN
    explicit CoPool(int co_max);
    virtual ~CoPool();

    virtual int             Submit(const CoPoolWorkCallback& workfunc);
    virtual int             Submit(CoPoolWorkCallback&& workfunc);

    /**
     * @brief 提交任务并返回携带结果的 future
     *        任务异常通过 future.get() 抛出，正常完成则 get() 返回。
     * @return 空 future 表示提交失败（池已停止或内存不足）
     */
    virtual std::future<void> SubmitWithFuture(const CoPoolWorkCallback& workfunc);
    virtual std::future<void> SubmitWithFuture(CoPoolWorkCallback&& workfunc);

    /**
     * @brief 设置任务异常回调。设置后异常交付回调（不再仅计数）；
     *        传 nullptr 恢复默认（仅计数）。
     */
    virtual void            SetExceptionCallback(std::function<void(std::exception_ptr)> callback);

    /**
     * @brief 未被回调/future 处理的异常数量（"忽略"策略的观测值）
     */
    virtual uint64_t        GetUnhandledExceptionCount() const noexcept;

    /**
     * @brief 阻塞直到，池中所有协程全部退出
     */
    virtual void            Release();
protected:
    void                    _Start();
    /* 工作协程 */
    void                    _WorkCo();
    /* 监控协程 */
    void                    _MonitorCo();
private:
    /* 处理单个任务的异常（统一隔离入口） */
    void                    _HandleWorkException(class Work* work, const std::exception_ptr& eptr) noexcept;

    std::atomic_bool        m_is_running{true};
    const int               m_max_co_num{0};
    moodycamel::ConcurrentQueue<Work*> 
                            m_works_queue;
    std::atomic_int         m_running_co_num{0};

    std::shared_ptr<sync::CoCond>
                            m_cond{nullptr};
    std::mutex              m_cond_mtx;

    std::mutex              m_exception_cb_mtx;
    std::function<void(std::exception_ptr)> m_exception_callback{nullptr};

    std::atomic_uint64_t    m_unhandled_exception_count{0};

    bbt::core::thread::CountDownLatch m_latch;

};

}; // namespace bbt::coroutine::pool
