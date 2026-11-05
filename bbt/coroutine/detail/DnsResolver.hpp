#pragma once
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

namespace bbt::coroutine::detail {

/** @brief DNS 阻塞解析的单 worker 任务池：getaddrinfo 等投到 worker 线程跑，协程经 CoWaiter 挂起。
 * Await 只能在协程内调用，work 跑在 worker 线程（禁止碰协程原语）且 work 与结果变量的寿命须覆盖整个等待；
 * 超时后晚到的结果由调用方丢弃。懒启动、进程寿命对象（无 Stop），队列满即拒绝入队。 */
class DnsResolver {
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
    sync::CombinedWaitStatus AwaitBounded(const std::function<void()>& work, const sync::CombinedWaitOptions& options);

private:
    static constexpr std::size_t kMaxPendingJobs = 256; // 排队上限：满了拒绝入队，不丢已接纳任务
    struct Job {
        std::function<void()> work;
        sync::CoWaiter::SPtr waiter;
    };

    DnsResolver() = default;
    ~DnsResolver() = default;

    // 入队并懒创建 worker；队列满或线程创建失败返回 false，任务不执行。
    bool Enqueue(Job job);
    void _Worker();

    std::mutex m_mutex;                    // 保护 m_queue 与 m_thread 的懒创建
    std::condition_variable m_cv;          // worker 在此等队列非空
    std::queue<Job> m_queue;               // 待执行任务，上限见 kMaxPendingJobs
    std::unique_ptr<std::thread> m_thread; // 首次 Enqueue 创建；进程寿命内不 join、不析构
};

} // namespace bbt/coroutine/detail
