/**
 * 协程对象 Coroutine：创建/恢复/让出/取消与诊断现场。
 */
#pragma once
#include <atomic>
#include <bbt/coroutine/detail/CfsRunQueue.hpp>
#include <exception>
#include <memory>
#include <mutex>
#include <vector>
#include <bbt/core/Attribute.hpp>
#include <bbt/coroutine/detail/interface/ICoroutine.hpp>
#include <bbt/coroutine/detail/Context.hpp>

namespace bbt::coroutine::sync {
class CoWaiter;
class CoMutex;
}

namespace bbt::coroutine::detail {

enum FollowEventStatus {
    DEFAULT = 0,
    TIMEOUT = 1,
    READABLE = 2,
};

/**
 * 仅用于 Processer 在上下文切回后决定 Coroutine 的交接方式。
 */
enum class CoroutineYieldDisposition {
    MANUAL,
    READY,
    EVENT_WAIT,
    FINAL,
};

/**
 * @brief 一个用户任务：内嵌 Context（借来的栈）与 CFS 节点，记录运行状态和当前等待事件。
 *
 * 所有权：Create 返回裸指针（Ptr 即 Coroutine*），没有 shared_ptr。Scheduler 入队后所有权交给
 * Processer：CommitYield 报 FINAL 由 Processer delete，报 READY 重新入队，报 EVENT_WAIT 交给
 * CommitPark 完成 park。同一时刻只有一个 Processer 线程操作它；m_await_event 由 m_await_mu 保护，
 * 所以 poller 派发和其他 worker 的 Notify 能在别的线程取到事件引用并 Trigger，运行状态只有持有者能改。
 * 状态机见 Define.hpp 的 CoroutineStatus 图。
 */
class Coroutine : public ICoroutine {
public:
    typedef Coroutine* Ptr;

    /**
     * sync 层 Wait 族与 CoMutex 等待路径均经同一登记路径挂起，必须能访问
     * _RegistAwaitEvent（取消预检），否则 custom 等待不响应预取消。
     * 仅开放给工具层等待原语，不扩大为公共 API。
     */
    friend class bbt::coroutine::sync::CoWaiter;
    friend class bbt::coroutine::sync::CoMutex;
    friend class Scheduler;
    friend class Processer;

    Coroutine(size_t stack_size, const CoroutineCallback& co_func, bool need_protect);
    Coroutine(StackLease stack, const CoroutineCallback& co_func);
    virtual ~Coroutine();

    /**
     * @brief 创建协程并分配 id；不启动、不入队。@p stack_size 为请求字节数，@p desc 为 nullptr 或空串都按未命名处理。
     * @return 裸指针，调用方负责入队或 delete；栈借不到、描述赋值 bad_alloc 都直接抛出。
     */
    static Ptr Create(size_t stack_size, const CoroutineCallback& co_func, bool need_protect = true,
                      const char* desc = nullptr);
    static Ptr Create(StackLease stack, const CoroutineCallback& co_func, const char* desc = nullptr);

    /**
     * @brief 唤醒并切进协程，返回时协程已再次让出。
     * 只能在持有该协程的 Processer 线程调用；运行状态不是 CO_RUNNABLE/CO_SUSPEND 时 Assert。
     */
    virtual void Resume() override;

    /**
     * @brief 主动让出，标记为 MANUAL + CO_SUSPEND 后切回调度线程。
     * 只能由协程自身在 CO_RUNNING 状态调用。MANUAL 不产生 await 事件，CommitYield 也不会把它重新入队。
     */
    virtual void Yield() override;

    /**
     * @brief 挂起协程，并在挂起成功后执行函数。
     * 因为 EventLoop 在 Scheduler 中运行，挂起前注册事件可能被立刻触发、协程还没让出就被唤醒，
     * 所以 cb 由 Resume 在协程真正让出之后执行，用来做事件注册这类必须晚于挂起的动作。
     * @return 0 成功；cb 判定失败返回 -1。
     */
    virtual int YieldWithCallback(const CoroutineOnYieldCallback& cb) override;

    /**
     * @brief 标记 READY 后让出，由 Processer 把它重新入队（本地或 global）；与事件等待不冲突，协程内逻辑串行。
     */
    virtual void YieldAndPushGCoQueue();

    /**
     * @brief Processer 在统计结清后调用，决定这次让出的去向。
     * 返回 FINAL 表示对象已到终态、可由调用方销毁；EVENT_WAIT 在这里提交 park，提交可能立即完成并唤醒协程。
     * 发布之后不得再次访问 Coroutine。
     */
    CoroutineYieldDisposition CommitYield();

    /**
     * @brief 协程 id，进程内单调递增，创建后不变。
     */
    virtual CoroutineId GetId() const noexcept override;

    /**
     * @brief 运行状态快照；由持有它的 Processer 线程写入，m_run_status 是 volatile 非原子，无同步的跨线程并发读不安全。
     */
    virtual CoroutineStatus GetStatus() const noexcept override;

    /**
     * @brief 最后一次导致唤醒的事件掩码（PollEventType 口径，含 TIMEOUT/CANCELLED/CLOSED 位）。
     * @return 尚未被事件唤醒过时返回 -1。
     */
    int GetLastResumeEvent() const noexcept;

    /**
     * @brief 本协程可用栈的字节数。
     */
    size_t GetStackSize() const noexcept;

#ifdef BBT_COROUTINE_STACK_DIAGNOSTICS

    /**
     * 同 Context::GetStackBudgetSample 的安全点前置条件：owner 协程在自身
     * 同步安全点调用，或调用方已同步确认协程不在运行（Resume 已返回/协程
     * 已让出或终态）；无同步的并发读禁止。PARTIAL 观测样本，不是栈用量
     * 上下界。
     */
    StackBudgetSample GetStackBudgetSample() const noexcept { return m_context.GetStackBudgetSample(); }
#endif

    /**
     * @brief 本次事件等待恢复后校验 fd 代际是否仍与登记时一致。
     *
     * 返回 true 表示安全可重试 syscall；false 表示 fd 在挂起期间被
     * close（epoch 已推进或事件以 POLL_EVENT_CLOSED 唤醒）——
     * 不得对 fd 数字再发起 syscall（可能已复用为新对象）。
     * 仅协程自身线程调用。
     */
    bool FdWaitEpochValid(int fd) const noexcept;

    /**
     * @brief 诊断现场（运行时契约观测性要求）
     *
     * GetDescription：注册时 bbtco_desc 携带的任务描述，未命名为空串。
     * GetWaitInfo：仅协程**自身**（同 Processer 线程）可安全读取——parked
     * 现场成员无锁，跨线程统一快照由 WorkerStallInfo 承载。未处于事件等待时返回 -1。
     */
    const std::string& GetDescription() const noexcept { return m_desc; }
    int GetWaitInfo(CoroutineWaitInfo& out) const noexcept;

    /**
     * @brief 记录异常并把协程推进到终态（FINAL），顺手取消尚未完成的 await 事件。
     * 在 Context 的 catch 和用户主函数的 catch 里都会调用；异常本身由 GetException 交付。
     */
    void OnException(std::exception_ptr eptr = nullptr) noexcept;

    /**
     * @brief 取保存的异常；未发生过异常时为空。
     */
    std::exception_ptr GetException() const noexcept;

    /**
     * 协作式取消：只置位并唤醒等待中的协程，不强杀运行中的用户代码。
     * 运行中的协程在检查点（IsCancelRequested / 等待返回后）自行退出，RAII 照常展开。
     */
    void RequestCancel() noexcept;

    /**
     * @brief 是否已请求取消；acquire 语义，配合 RequestCancel 的 release 发布。
     */
    bool IsCancelRequested() const noexcept;

    /**
     * MLFQ 调度支持：OnCoPollEvent 按 1ms 时间片决定唤醒优先级，超时降级（最多 3 次），时长收敛则逐次恢复。
     */
    uint64_t GetLastRunTimeUs() const noexcept { return m_last_run_us; }
    void SetLastRunTimeUs(uint64_t us) { m_last_run_us = us; }
    int GetMlfqDemotions() const noexcept { return m_mlfq_demotions; }
    void IncrementDemotions() { m_mlfq_demotions++; }
    void ResetDemotions() { m_mlfq_demotions = 0; }

    /**
     * 事件等待族：先在 m_await_event 备好 CoPollEvent，再经 YieldWithCallback 在挂起后注册——注册早于
     * 让出的话 poller 可能在协程还没让出时就把它标成就绪。唤醒来源不止两种：Scheduler 派发就绪事件、
     * 其他 Processer 触发 custom 事件、native Notify、取消与 close 都会推进完成；都只入队，运行状态
     * 保持 CO_SUSPEND 直到再次 Resume。
     * 返回码：0 = 已注册并在被唤醒后返回（原因看 GetLastResumeEvent）；-1 = 事件创建/初始化/注册失败，
     * 协程未真正挂起；1 = 进入时已有取消请求（未挂起）。同一时刻最多一个 await_event，已有事件时 Assert。
     */
    int YieldUntilTimeout(int ms);

    /**
     * @brief 登记一个 custom 事件，由外部 Trigger 唤醒；已有 await_event 或初始化失败返回 nullptr。
     */
    std::shared_ptr<CoPollEvent> RegistCustom(int key);

    /**
     * @brief 同上，另加超时；超时与 custom 谁先到都算唤醒。
     */
    std::shared_ptr<CoPollEvent> RegistCustom(int key, int timeout_ms);
    int YieldUntilFdReadable(int fd);
    int YieldUntilFdReadable(int fd, int timeout_ms);
    int YieldUntilFdWriteable(int fd);
    int YieldUntilFdWriteable(int fd, int timeout_ms);
    int YieldUntilFdEx(int fd, short events, int timeout_ms = 0);

    /**
     * @brief 单测故障注入：置位后本进程下一次带 await 事件的
     *  _RegistAwaitEvent 先把该事件 UnRegist 成 CANCELLED，再走真实
     *  Regist() → -1（事件注册阶段失败，非 RegistCustom 创建阶段），
     *  YieldWithCallback 因此返回 -1 且协程未真正挂起。用于复现"登记
     *  失败时旧返回码误读上次唤醒掩码"的回归。
     *
     *  只应在单测中使用；标志一次性消费（exchange 清零），不影响后续
     *  等待。无任何并发语义保证之外的副作用，生产路径代价是一次原子读。
     */
    static void _TestFailNextAwaitRegist() noexcept;

protected:
    /**
     * 唤醒入口：归一化掩码、清 await、按 MLFQ/超时规则定优先级后交给 Scheduler 入队。执行线程不固定——
     * PENDING 由 CommitPark 所在 Processer 完成，PARKED 由 Trigger 调用线程同步完成。
     */
    void OnCoPollEvent(int event, int custom_key);

protected:
    /**
     * 进程内单调递增，从 INVALID_COROUTINE_ID 之后开始
     */
    static CoroutineId _GenCoroutineId();

private:
    /**
     * 挂起后注册当前 await 事件：先做取消预检与单测注入，注册失败则清 await、回滚 disposition。
     */
    bool _RegistAwaitEvent();

    /**
     * 锁内拷贝一份 shared_ptr：调用方拿到后即使槽被清空，事件对象仍然存活。
     */
    std::shared_ptr<CoPollEvent> _AwaitEvent() const;

    /**
     * 按值接管所有权；传 nullptr 即清空槽位。
     */
    void _SetAwaitEvent(std::shared_ptr<CoPollEvent> ev);

    /**
     * 单测故障注入标志：见 _TestFailNextAwaitRegist 注释
     */
    static std::atomic_bool s_test_fail_await_regist;

private:
    /**
     * CFS 节点，随本对象生灭
     */
    CfsEntity m_cfs;
    Context m_context;
    const CoroutineId m_id{BBT_COROUTINE_INVALID_COROUTINE_ID};

    /**
     * 只有持有它的 Processer 线程写
     */
    volatile CoroutineStatus m_run_status{CoroutineStatus::CO_DEFAULT};

    /**
     * 当前等待事件：挂起前建好、挂起后注册，唤醒时被消费清空。
     * 由 m_await_mu 保护，所以 poller 派发和其他 worker 的 Notify 能取到引用并 Trigger。
     */
    std::shared_ptr<CoPollEvent> m_await_event{nullptr};
    mutable std::mutex m_await_mu;
    std::atomic_bool m_cancel_requested{false};
    std::exception_ptr m_exception;
    CoroutineOnYieldCallback m_co_onyield_callback{nullptr};

    /**
     * 本次让出的去向，由 Coroutine::Yield* 写入、CommitYield 读出后交给 Processer。
     */
    CoroutineYieldDisposition m_yield_disposition{CoroutineYieldDisposition::MANUAL};

    /**
     * 最后一次导致此协程唤醒的事件（PollEventType 归一化掩码）
     */
    int m_last_resume_event{-1};

    /**
     * 恢复时由 OnCoPollEvent 从消费掉的 await_event 捕获，供
     * FdWaitEpochValid 在重试 syscall 前比对；仅协程自身线程读写。
     */
    int m_last_wait_fd{-1};
    uint64_t m_last_wait_epoch{0};

    /**
     * 上次运行时长（微秒），用于 MLFQ
     */
    uint64_t m_last_run_us{0};

    /**
     * MLFQ 连续降级次数
     */
    int m_mlfq_demotions{0};

    /**
     * bbtco_desc 任务描述
     */
    std::string m_desc{};

    /**
     * 进入事件等待的时刻
     */
    uint64_t m_parked_us{0};
};

}
