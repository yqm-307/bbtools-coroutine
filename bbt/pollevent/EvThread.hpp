/**
 * @file bbt/pollevent/EvThread.hpp
 * @brief 一条 OS 线程 + 一个 EventLoop 的 IO 线程封装；事件回调在该 IO 线程执行，须先 Start() 再析构。
 */
#pragma once
#include <thread>
#include <memory>

#include <bbt/pollevent/detail/Define.hpp>

namespace bbt::pollevent {

/**
 * @brief 一条 IO 线程 + 一个 EventLoop，在自身线程内驱动派发；Start/Stop/Join 非线程安全。
 */
class EvThread final : public std::enable_shared_from_this<EvThread> {
public:
    typedef std::shared_ptr<EvThread> SPtr;

    /**
     * @brief 接管外部 EventLoop，不创建线程；@p evloop 必须非空。
     */
    explicit EvThread(std::shared_ptr<EventLoop> evloop);

    /**
     * @brief 自建 EventLoop，不创建线程。
     */
    EvThread();

    /**
     * @brief 析构时调用 Stop()；已 Finish 则 Stop 直接返回，不回收线程句柄。
     */
    virtual ~EvThread();

    /**
     * @brief 启动 IO 线程（非线程安全）；已 Running 时直接返回，Finish 状态不代表句柄已回收，不保证能再次 Start。
     */
    void Start();

    /**
     * @brief 停止 IO 线程（非线程安全）：BreakLoop → join → 销毁线程句柄。
     * @return 恒为 std::nullopt（BreakLoop 恒返回 0）；须已 Start()，否则等待路径解引用空线程句柄。
     */
    core::errcode::ErrOpt Stop();

    /**
     * @brief 进程内自增的 IO 线程 id（≠ std::thread::id），恒 >= 1。
     */
    EvThreadId GetTid() const;

    /**
     * @brief 观测是否 Running；m_status 是 volatile，不能当作线程已启动/已退出的同步点。
     */
    bool IsRunning() const;

    /**
     * @brief 在 EventLoop 上创建事件（线程安全）；只是 CreateEvent，须再调 Event::StartListen 才进入等待。
     * @param fd 用户 fd；-1 表示定时器或 Trigger() 事件；回调在 IO 线程内执行。
     */
    std::shared_ptr<Event> RegisterEvent(evutil_socket_t fd, short events,
                                         const bbt::pollevent::OnEventCallback& onevent_cb);

    /**
     * @brief 阻塞 join，不请求停止；须持有可 join 的线程句柄，Stop 若已走清理路径销毁句柄，之后不可调用。
     */
    void Join();

    /**
     * @brief 本对象持有的 EventLoop，恒非空。
     */
    std::shared_ptr<EventLoop> GetEventLoop() const;

protected:
    /**
     * 子类可覆写的线程主体；默认实现驱动 EventLoop 直到 BreakLoop。
     */
    virtual void WorkHandle();

    /**
     * 取 std::thread 的系统 id；线程未启动时 m_thread 为空。
     */
    std::thread::id GetSysTid() const;

    /**
     * 创建 std::thread；已有线程对象时直接返回。
     */
    void StartWorkFunc();

    /**
     * 无限等待线程退出（内部 join）。
     */
    void SyncWaitThreadExit();

    /**
     * 有界等待：只有负数参数真正 join；正数路径仅轮询 joinable 后返回。
     */
    bool SyncWaitThreadExitWithTime(int wait_time);

private:
    void Destory();
    void Work();
    bool SyncWaitThreadExitEx(int wait_time);
    static EvThreadId GenerateTid();

private:
    std::shared_ptr<bbt::pollevent::EventLoop> m_eventloop{nullptr};

    /**
     * 堆上的线程对象：Stop() 里 delete，析构前必须已 join。
     */
    std::thread* m_thread{nullptr};

    /**
     * 进程内自增的 IO 线程 id，小于 0 是非法值（默认 -1 表示未分配）。
     */
    EvThreadId m_tid{-1};

    /**
     * 由 Work() 写、其它线程只读；volatile 不提供跨线程同步。
     */
    volatile EvThreadRunStatus m_status{EvThreadRunStatus::Default};
};

}
