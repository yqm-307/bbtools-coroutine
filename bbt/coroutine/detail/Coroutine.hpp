#pragma once
#include <memory>
#include <bbt/core/Attribute.hpp>
#include <bbt/coroutine/detail/interface/ICoroutine.hpp>
#include <bbt/coroutine/detail/Context.hpp>

namespace bbt::coroutine::detail
{

enum FollowEventStatus
{
    DEFAULT = 0,
    TIMEOUT = 1,
    READABLE = 2,
};

/**
 * @brief
 *
 * Coroutine是一个协程对象，封装了协程的上下文和状态信息。
 * 
 * Coroutine还提供了常用的挂起、唤醒以及基于事件的挂起和唤醒的功能。 
 * 
 * Coroutine使用shared_ptr来控制生命期，其主要目的是为了在使用
 * 基于EventLoop的调度器中不会被意外释放。减少编写调度器的难度，
 * 但是目前Scheduler和Processer的实现可以保证一个Coroutine只
 * 会在一个时刻，被一个Processer访问到，因为所有Coroutine执行都
 * 在Processer的上下文中进行。
 * 
 * TODO ： 不再使用shared_ptr来控制Coroutine的生命周期
 * 
 */
class Coroutine:
    public ICoroutine
{
public:
    typedef Coroutine* Ptr;

    Coroutine(int stack_size, const CoroutineCallback& co_func, bool need_protect);
    virtual ~Coroutine();
    
    static Ptr                     Create(int stack_size, const CoroutineCallback& co_func, bool need_protect = true);
    /**
     * @brief 唤醒协程。切换到协程的上下文中执行。
     */
    virtual void                    Resume() override;

    /**
     * @brief 挂起协程。保存协程上下文，并回到调用Resume的上下文中
     */
    virtual void                    Yield() override;

    /**
     * @brief 挂起协程，并在挂起成功后执行函数
     * 
     * 因为协程在Processer中执行，但是EventLoop在Scheduler中运行。
     * 如果当前Coroutine在挂起前注册事件，可能会导致Scheduler中
     * 立即触发此事件并将Coroutine放入就绪队列，导致当前Coroutine
     * 还没被挂起就被唤醒了。
     * 
     * 因此这个函数是为了在挂起成功后，保证Event同步操作的。
     * 
     * @param cb 在挂起成功后执行的函数
     * @return int 成功返回0，cb执行失败返回-1
     */
    virtual int                     YieldWithCallback(const CoroutineOnYieldCallback& cb) override;
    /* 手动挂起co并加入到全局队列中，和事件触发挂起无冲突，因为一个协程内逻辑串行 */

    /**
     * @brief 挂起协程，并将协程加入到全局就绪队列中去
     */
    virtual void                    YieldAndPushGCoQueue();

    virtual CoroutineId             GetId() noexcept override;
    CoroutineStatus                 GetStatus() const noexcept;
    int                             GetLastResumeEvent() const noexcept;
    size_t                          GetStackSize() const noexcept;

    /**
     * YieldUnitl事件
     * 
     * 这些事件是为了让Coroutine支持事件驱动的挂起和唤醒。
     * 
     * 工作原理：
     * 首先Coroutine只能在Processer中被访问，当Coroutine执行YieldUnitl被挂起时，
     * Coroutine会被存储在Event中，此时Coroutine就会被挂起了。Processer会继续执
     * 行其他Coroutine。当Scheduler中发现有Coroutine的等待事件就绪了，就会将对应
     * 的Coroutine放入到就绪队列中去。Coroutine只能在一个Processer中被访问，所以
     * Coroutine可以保证事件是唯一的，不会出现同一个Coroutine被多次唤醒的情况。
     * 
     *  事件触发有两种情况：
     *      1、Scheduler中事件就绪了，会将Coroutine放入就绪队列中去；
     *      2、其他Processer中触发了Custom事件，会将Coroutine放入就绪队列中去。
     * 
     * 支持的事件有：
     * - 可读事件：当fd可读
     * - 可写事件：当fd可写
     * - 超时事件：当超过指定时间
     * - 自定义事件：当自定义事件被触发
     * 
     * Q: 事件触发是否为异步的？
     * A: 是的，首先明确Coroutine只能在Processer中被访问，且YieldUnitl
     * 只能由当前Coroutine主动调用，那么事件注册的唯一性就可以保证了。
     * 
     */

    int                             YieldUntilTimeout(int ms);
    std::shared_ptr<CoPollEvent>    RegistCustom(int key);
    std::shared_ptr<CoPollEvent>    RegistCustom(int key, int timeout_ms);
    int                             YieldUntilFdReadable(int fd);
    int                             YieldUntilFdReadable(int fd, int timeout_ms);
    int                             YieldUntilFdWriteable(int fd);
    int                             YieldUntilFdWriteable(int fd, int timeout_ms);

protected:
    void                            OnCoPollEvent(int event, int custom_key);

protected:
    static CoroutineId              _GenCoroutineId();

private:
    Context                         m_context;
    const CoroutineId               m_id{BBT_COROUTINE_INVALID_COROUTINE_ID};
    volatile CoroutineStatus        m_run_status{CoroutineStatus::CO_DEFAULT};

    /**
     * 这是协程调度的核心
     * 
     * 当协程挂起时，会创建一个await_event事件，此事件会在满足条件后触发
     * 并使当前协程进入就绪态并 push 到 global_queue 中。
     * 
     * 支持的事件有：
     *  1. 自定义事件（由用户手动触发，如CoCond::Notify）
     *  2. 超时事件（当超过指定时间后触发，libevent提供）
     *  3. 可读事件（当fd可读时触发，libevent提供）
     *  4. 可写事件（当fd可写时触发，libevent提供）
     * 
     */
    std::shared_ptr<CoPollEvent>    m_await_event{nullptr};
    CoroutineOnYieldCallback        m_co_onyield_callback{nullptr};

    int                             m_last_resume_event{-1};    // 最后一次导致此协程唤醒的事件
};

}