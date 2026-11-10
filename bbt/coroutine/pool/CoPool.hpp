#pragma once
#include <boost/noncopyable.hpp>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/runtime/RuntimeOptions.hpp>
#include <bbt/coroutine/utils/lockfree/concurrentqueue.h>

#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

namespace bbt::coroutine::pool
{

/**
 * @brief 一个尺寸类的不可变配置。
 *
 * request 规范化后作为路由键 (U, protect)：worker 栈规格与提交键必须完全一致，
 * 池不会用相邻尺寸或另一保护属性的 worker 顶替。worker_num 与 queue_capacity
 * 都是有限值，池不会为单个任务临时建栈或无限排队。
 */
struct CoPoolClassConfig {
    StackRequest request{0, false};
    std::size_t worker_num{0};
    std::size_t queue_capacity{0};
};

/** @brief 尺寸路由池的创建参数；各类规范化后的键必须两两不同。 */
struct CoPoolOptions {
    std::vector<CoPoolClassConfig> classes;
};

/** @brief 尺寸路由提交结果；拒绝原因不合并，调用方可区分重试与用法错误。 */
enum class CoPoolSubmitStatus {
    Accepted,
    NoExactClass,     ///< 没有精确匹配的 (U, protect) 类；未携带尺寸的提交同样落在这里
    QueueFull,        ///< 该类有界队列已满
    Released,         ///< 池已封口，不再接纳新任务
    AllocationFailed, ///< 任务节点或队列节点分配失败
};

/**
 * @brief 协程池，池子会创建固定数量的协程来处理任务。
 *
 * 两种模式共用同一套 worker/关闭/异常语义：
 * 1. `Create(max_co)`：兼容单队列模式，所有 worker 使用默认栈，队列无界；
 * 2. `Create(CoPoolOptions)`：尺寸路由模式，每个 worker 类有不可变 `StackRequest`
 *    与固定数量，`Submit(StackRequest, …)` 只投递到精确匹配的类，队列有界。
 *
 * 注意：
 * 1. 协程池的协程数量是固定的，不能动态增加；
 * 2. 在协程池中执行任务，如果执行阻塞操作导致协程挂起，会占用这个协程，
 *    导致池子中可用协程数变少；
 * 3. 池必须活过全部 worker 与在途任务；worker 只持有池的裸指针。
 * 4. 构造（含失败回收）与 `Release` 都要求 runtime 可被推进：THREAD/LOOP 由后台线程推进，
 *    NO_LOOP 必须由 native 侧已经在驱动 `LoopOnce`，否则等待 worker 退出不会返回。
 *
 * 活性前提：禁止同一池内的等待依赖（任务 A 等待提交到同一池的任务 B）。
 * 池不做死锁检测；有等待依赖的模块要用独立类或独立池。
 *
 * 异常语义：
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

    /**
     * @brief 创建尺寸路由池。
     * @throw std::invalid_argument 空类列表、0 worker/队列容量、规范化后重复的类
     * @throw std::runtime_error 类或 monitor 的 worker 栈超出当前 StackPool 预算，
     *        或 worker 创建失败；已创建的 worker 全部回收后才抛出，不留半成品池
     */
    static std::shared_ptr<CoPool> Create(const CoPoolOptions& options);

    BBTATTR_FUNC_CTOR_HIDDEN
    explicit CoPool(int co_max);
    BBTATTR_FUNC_CTOR_HIDDEN
    explicit CoPool(const CoPoolOptions& options);
    virtual ~CoPool();

    virtual int             Submit(const CoPoolWorkCallback& workfunc);
    virtual int             Submit(CoPoolWorkCallback&& workfunc);

    /**
     * @brief 按精确栈规格提交到匹配的 worker 类；未匹配/满队列/封口都立即拒绝。
     *
     * 只路由到规范化后 (U, protect) 完全一致的类，不跨尺寸兜底，也不排队等预算。
     */
    CoPoolSubmitStatus Submit(StackRequest request, const CoPoolWorkCallback& workfunc);
    CoPoolSubmitStatus Submit(StackRequest request, CoPoolWorkCallback&& workfunc);

    /**
     * @brief 提交任务并返回携带结果的 future
     *        任务异常通过 future.get() 抛出，正常完成则 get() 返回。
     * @return 空 future 表示提交失败（池已停止或内存不足）；尺寸路由池没有
     *         默认尺寸，未携带栈规格的 future 提交同样返回空 future
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
     * @brief 停止接收新任务（Submit* 返回 -1/拒绝），等待池中协程退出，
     *        并取消式排空队列滞留任务（不执行；带 future 的任务以
     *        broken_promise 兑现，调用方不会永挂）。幂等，可重复调用。
     * @throw std::logic_error 在池自身 worker 协程内调用（自等待，必然死锁）；
     *        该检查发生在任何关闭副作用之前
     */
    virtual void            Release();
protected:
    void                    _Start();
    /* 工作协程 */
    void                    _WorkCo();
    /* 监控协程 */
    void                    _MonitorCo();
private:
    /* 尺寸类的运行态在 .cc 定义；这里只前置声明，供下面的私有声明使用。 */
    struct ClassState;

    /* 尺寸类的固定 worker：只消费本类队列 */
    void _WorkCoRoute(ClassState& state);
    /* 两种模式共用的 worker/monitor 登记与退出记账；失败时回退 created 计数 */
    void _StartWorker(StackRequest request, ClassState* state, const char* desc);
    void _StartMonitor(StackRequest request, const char* desc);
    /* 尺寸路由模式的构造期创建：先校验/容量查询，再逐个建 worker，失败自回收 */
    void _StartRouted(const CoPoolOptions& options);
    /* 构造失败自回收：封口并等到已登记 worker 离开，之后才允许析构 half-built 池 */
    void _AbortStart() noexcept;
    /* 封口后等待已登记 worker/monitor 真实退出；Co 与 native 域都可调用 */
    void _WaitWorkersExit();
    /* 拒绝在池自身 worker 内关闭：这里没有可等待的执行域 */
    bool _IsInPoolWorker() const;
    void _RecordWorkerId();
    ClassState* _FindClass(StackRequest request) const;
    CoPoolSubmitStatus _Enqueue(ClassState& state, class Work* work);
    /* 执行单个任务并隔离其异常；future 路径同时兑现结果 */
    void _RunWork(class Work* work) noexcept;
    /* 封口后删除滞留任务；delete 释放 promise 形成 broken_promise */
    void _DrainQueues() noexcept;
    /* 处理单个任务的异常（统一隔离入口） */
    void                    _HandleWorkException(class Work* work, const std::exception_ptr& eptr) noexcept;

    std::atomic_bool        m_is_running{true};
    const int               m_max_co_num{0};
    moodycamel::ConcurrentQueue<Work*> 
                            m_works_queue;
    /* 非空表示尺寸路由模式；兼容模式保持单队列语义 */
    std::vector<std::unique_ptr<ClassState>>
        m_classes;

    std::shared_ptr<sync::CoCond>
                            m_cond{nullptr};
    std::mutex              m_cond_mtx;

    /* worker 从登记到退出的账目：Release 必须等到每个已登记 worker 真实离开，
     * 否则池成员析构时仍有协程引用 this。 */
    std::atomic_size_t m_created_workers{0};
    std::atomic_size_t m_exited_workers{0};

    /* 池内 worker 身份用于自等待拒绝；协程可能在任意 worker 线程恢复，
     * 所以用协程 id 而不是线程局部标记。 */
    mutable std::mutex m_worker_ids_mutex;
    std::vector<detail::CoroutineId>
        m_worker_ids;

    std::mutex              m_exception_cb_mtx;
    std::function<void(std::exception_ptr)> m_exception_callback{nullptr};

    std::atomic_uint64_t    m_unhandled_exception_count{0};

};

}; // namespace bbt::coroutine::pool
