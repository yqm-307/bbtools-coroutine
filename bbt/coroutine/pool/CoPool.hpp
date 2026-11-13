/**
 * @file CoPool.hpp
 * @brief 固定并发度的协程池：任务经队列投递给池内协程，异常在池内隔离后交付。
 */
#pragma once
#include <boost/noncopyable.hpp>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/pool/CoPoolTypes.hpp>
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

namespace bbt::coroutine::pool {

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
class CoPool : boost::noncopyable {
public:
    /**
     * @brief 创建池并注册 max_co 个 work 协程 + 1 个 monitor；须在 runtime 初始化后调用，max_co 须 > 0（断言）。
     * @return 池的共享 owner；无可用 worker 抛 std::runtime_error，栈申请失败抛 length_error/bad_alloc。
     */
    static std::shared_ptr<CoPool> Create(int max_co);

    /**
     * @brief 创建尺寸路由池。
     * @throw std::invalid_argument 空类列表、0 worker/队列容量、规范化后重复的类
     * @throw std::runtime_error 类或 monitor 的 worker 栈超出当前 StackPool 预算，
     *        或 worker 创建失败；已创建的 worker 全部回收后才抛出，不留半成品池
     */
    static std::shared_ptr<CoPool> Create(const CoPoolOptions& options);

    /**
     * @brief 直接构造并注册池内协程，通常走 Create；co_max 必须 > 0（断言）
     */
    BBTATTR_FUNC_CTOR_HIDDEN
    explicit CoPool(int co_max);
    BBTATTR_FUNC_CTOR_HIDDEN
    explicit CoPool(const CoPoolOptions& options);

    /**
     * @brief 析构：丢弃队列中滞留任务，再 Release 等池内协程退出
     */
    virtual ~CoPool();

    /**
     * @brief 投递任务，由池内空闲协程执行；只入队，不阻塞调用方。
     * @return 0 入队成功；-1 池已停止、分配失败或入队失败，任务不会被执行
     */
    virtual int Submit(const CoPoolWorkCallback& workfunc);

    /**
     * @brief 同上，接受右值回调（可移入捕获的 lambda）
     */
    virtual int Submit(CoPoolWorkCallback&& workfunc);

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

    /**
     * @brief 同上，接受右值回调
     */
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
     * @brief 停止接收新任务（Submit* 返回 -1/拒绝），等待池中协程退出，
     *        并取消式排空队列滞留任务（不执行；带 future 的任务以
     *        broken_promise 兑现，调用方不会永挂）。幂等，可重复调用。
     * @throw std::logic_error 在池自身 worker 协程内调用（自等待，必然死锁）；
     *        该检查发生在任何关闭副作用之前
     */
    virtual void Release();

protected:
    /**
     * 注册 m_max_co_num 个 work 协程和一个 monitor 协程；只在构造时调用一次
     */
    void _Start();

    /**
     * 工作协程主循环：取任务、执行、异常隔离；退出账目由 _StartWorker 的协程体记录
     */
    void _WorkCo();

    /**
     * 监控协程主循环：每 1ms 检查队列并补唤醒漏掉通知的 work 协程
     */
    void _MonitorCo();

private:
    /**
     * 尺寸类的运行态在 .cc 定义；这里只前置声明，供下面的私有声明使用。
     */
    struct ClassState;

    /**
     * 尺寸类的固定 worker：只消费本类队列
     */
    void _WorkCoRoute(ClassState& state);

    /**
     * 两种模式共用的 worker/monitor 登记与退出记账；失败时回退 created 计数
     */
    void _StartWorker(StackRequest request, ClassState* state, const char* desc);
    void _StartMonitor(StackRequest request, const char* desc);

    /**
     * 尺寸路由模式的构造期创建：先校验/容量查询，再逐个建 worker，失败自回收
     */
    void _StartRouted(const CoPoolOptions& options);

    /**
     * 构造失败自回收：封口并等到已登记 worker 离开，之后才允许析构 half-built 池
     */
    void _AbortStart() noexcept;

    /**
     * 封口后等待已登记 worker/monitor 真实退出；Co 与 native 域都可调用
     */
    void _WaitWorkersExit();

    /**
     * 拒绝在池自身 worker 内关闭：这里没有可等待的执行域
     */
    bool _IsInPoolWorker() const;
    void _RecordWorkerId();
    ClassState* _FindClass(StackRequest request) const;
    CoPoolSubmitStatus _Enqueue(ClassState& state, class Work* work);

    /**
     * 执行单个任务并隔离其异常；future 路径同时兑现结果
     */
    void _RunWork(class Work* work) noexcept;

    /**
     * 封口后删除滞留任务；delete 释放 promise 形成 broken_promise
     */
    void _DrainQueues() noexcept;

    /**
     * 处理单个任务的异常（统一隔离入口）：future 路径优先，其次回调，最后只计数
     */
    void _HandleWorkException(class Work* work, const std::exception_ptr& eptr) noexcept;

    /**
     * 基础设施异常不能按任务异常继续运行：封口、计数并唤醒所有等待 worker。
     */
    void _HandleWorkerException(const std::exception_ptr& eptr) noexcept;

    /**
     * Release 置 false 后，work/monitor 协程在下一轮循环条件处退出
     */
    std::atomic_bool m_is_running{true};
    const int m_max_co_num{0};

    /**
     * Work* 的所有权在这里：入队即交给池，由取到它的 work 协程 delete
     */
    moodycamel::ConcurrentQueue<Work*> m_works_queue;

    /**
     * 非空表示尺寸路由模式；兼容模式保持单队列语义
     */
    std::vector<std::unique_ptr<ClassState>> m_classes;

    /**
     * work 协程无任务时的等待点；入队方 NotifyOne 唤醒一个
     */
    std::shared_ptr<sync::CoCond> m_cond{nullptr};
    std::mutex m_cond_mtx;

    /**
     * worker 从登记到退出的账目：Release 必须等到每个已登记 worker 真实离开，
     * 否则池成员析构时仍有协程引用 this。
     */
    std::atomic_size_t m_created_workers{0};
    std::atomic_size_t m_exited_workers{0};

    /**
     * 池内 worker 身份用于自等待拒绝；协程可能在任意 worker 线程恢复，
     * 所以用协程 id 而不是线程局部标记。
     */
    mutable std::mutex m_worker_ids_mutex;
    std::vector<detail::CoroutineId> m_worker_ids;

    /**
     * 保护 m_exception_callback。取用时在锁内拷贝到局部、调用放在锁外：回调里若再调
     * SetExceptionCallback 会拿同一把非递归 mutex，持锁调用就自锁
     */
    std::mutex m_exception_cb_mtx;
    std::function<void(std::exception_ptr)> m_exception_callback{nullptr};

    /**
     * 只由 fetch_add 递增，供 GetUnhandledExceptionCount 读取
     */
    std::atomic_uint64_t m_unhandled_exception_count{0};
};

};
