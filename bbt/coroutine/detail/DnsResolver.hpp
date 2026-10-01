#pragma once
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

namespace bbt::coroutine::detail
{

/**
 * @brief DNS 阻塞解析的单线程任务池（#231）
 *
 * getaddrinfo/getnameinfo/gethostbyname/gethostbyaddr 会走真实网络或 NSS 查询，
 * 在协程内直接执行会占死 Processer 线程。本类把这类调用投递到唯一的 worker 线程
 * 执行，协程通过 CoWaiter 挂起，worker 完成后从非协程线程 Notify 唤醒。
 *
 * 时序约束：
 * - Await 只允许在协程上下文调用；work() 里禁止碰协程原语（跑在 worker 线程）。
 * - work 引用的结果变量在调用方协程栈上，挂起期间保持有效；调用方必须自行
 *   保证 work 与结果的寿命覆盖整个等待（超时后晚到的结果由调用方清理，
 *   禁止引用已退栈的协程栈）。
 * - 懒启动：第一次 Enqueue 才创建线程。
 * - 进程寿命：本池属于进程寿命对象，没有 Stop/停机路径；worker 线程与其
 *   std::thread 句柄都不会在静态退出期析构（GetInstance 持有者泄漏），
 *   因此不存在「析构 joinable thread」问题，也不以 detach 代替寿命论证。
 * - 排队数有上限；容量耗尽时拒绝入队而不丢弃已接纳任务。
 *
 * ponytail: 单 worker，QPS 不够再加池。无解析缓存。
 */
class DnsResolver
{
public:
    static DnsResolver* GetInstance();

    DnsResolver(const DnsResolver&) = delete;
    DnsResolver& operator=(const DnsResolver&) = delete;

    /**
     * @brief 在协程内挂起，把 work 投递到 worker 线程执行完毕后唤醒
     *
     * @param work 真正的阻塞调用；结果由 work 写回调用方捕获的变量
     * @return int 0 表示 work 已执行（成败看调用方自己的错误码变量），
     *             -1 表示未能入队（队列已满），work 不会执行
     */
    int Await(const std::function<void()>& work);
    /**
     * @brief 同一 DNS worker 执行 work，按单调绝对期限返回首胜原因。
     * @note 超时只终止调用方等待；已开始的 libc 工作无法强杀，work 必须
     *       自行拥有跨等待的参数与结果寿命。
     *       Completed 仅表示已唤醒：调用方须预置失败结果，超时/取消后
     *       晚到的 work 结果由调用方负责丢弃。
     *       队列满时返回 RuntimeUnavailable，work 不执行。
     */
    sync::CombinedWaitStatus AwaitBounded(
        const std::function<void()>& work,
        const sync::CombinedWaitOptions& options);

private:
    static constexpr std::size_t kMaxPendingJobs = 256;
    struct Job
    {
        std::function<void()> work;
        sync::CoWaiter::SPtr  waiter;
    };

    DnsResolver() = default;
    ~DnsResolver() = default;

    bool Enqueue(Job job);
    void _Worker();

    std::mutex                m_mutex;
    std::condition_variable   m_cv;
    std::queue<Job>           m_queue;
    std::unique_ptr<std::thread> m_thread;
};

} // namespace bbt/coroutine/detail
