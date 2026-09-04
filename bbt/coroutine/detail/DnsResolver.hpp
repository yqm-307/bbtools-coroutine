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
 * - work 引用的结果变量在调用方协程栈上，挂起期间保持有效；因此
 *   Scheduler::Stop 必须先 Stop 本池（join worker、唤醒全部排队 Job），
 *   再销毁 Processer——否则 work 写回/Notify 时调用方栈可能已析构。
 * - 懒启动：第一次 Enqueue 才创建线程；Stop 后不再允许重启（后续任务直接失败）。
 *
 * ponytail: 单 worker，QPS 不够再加池。无解析缓存、无超时（issue #231 本期不做）。
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
     *             -1 表示未能入队（池已停止），work 不会执行
     */
    int Await(const std::function<void()>& work);

    /**
     * @brief 置停、唤醒 worker、join 线程；队列剩余 Job 不执行 work、只 Notify 唤醒
     *
     * 可重复调用（幂等）。join 会等当前 in-flight 的 libc 调用返回。
     */
    void Stop();

private:
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
    bool                      m_stopping{false}; // 置停后拒绝新任务
};

} // namespace bbt/coroutine/detail
