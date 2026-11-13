#include <atomic>
#include <bbt/pollevent/EvThread.hpp>
#include <bbt/pollevent/EventLoop.hpp>

using namespace bbt::pollevent::detail;

namespace bbt::pollevent {

static const int io_thread_limit_num = 16;

EvThreadId EvThread::GenerateTid()
{
    /**
     * 进程内自增，从 1 开始；只做标识，不参与任何调度判定。
     */
    static std::atomic_int _id = 0;
    return ++_id;
}

EvThread::EvThread(std::shared_ptr<EventLoop> evloop) : m_eventloop(evloop), m_tid(GenerateTid())
{
    /**
     * 只记引用，不建线程：Start() 之前 IsRunning() 为 false，Stop()/Join() 不成立。
     */
}

EvThread::EvThread() : m_eventloop(std::make_shared<EventLoop>()), m_tid(GenerateTid()) {}

EvThread::~EvThread()
{
    Stop();
}

void EvThread::Destory()
{
    delete m_thread;
    m_thread = nullptr;
}

EvThreadId EvThread::GetTid() const
{
    return m_tid;
}

std::shared_ptr<EventLoop> EvThread::GetEventLoop() const
{
    return m_eventloop;
}

std::thread::id EvThread::GetSysTid() const
{
    /**
     * 线程未启动时 m_thread 为空，此处会解引用空指针；只在 Start() 之后调用。
     */
    return m_thread->get_id();
}

void EvThread::StartWorkFunc()
{
    /**
     * 幂等：已有线程对象就不再建，避免同一 EvThread 出现两条驱动线程。
     */
    if (m_thread != nullptr)
        return;

    m_thread = new std::thread([=]() { Work(); });
}

void EvThread::Work()
{
    /**
     * 线程入口：先置 Running 再驱动循环，退出时置 Finish；这两次写与 IsRunning() 的读之间没有同步。
     */
    m_status = EvThreadRunStatus::Running;
    WorkHandle();
    m_status = EvThreadRunStatus::Finish;
}

bool EvThread::IsRunning() const
{
    return (m_status == EvThreadRunStatus::Running);
}

std::shared_ptr<Event> EvThread::RegisterEvent(evutil_socket_t fd, short events,
                                               const bbt::pollevent::OnEventCallback& onevent_cb)
{
    /**
     * 只是 CreateEvent：不登记等待，事件在 StartListen 时才挂到 context 上。
     */
    auto event_sptr = m_eventloop->CreateEvent(fd, events, onevent_cb);
    return event_sptr;
}

void EvThread::Join()
{
    /**
     * 只 join，不请求停止：驱动循环还在跑时本调用不会返回；句柄须仍可 join（Stop 走清理路径后已销毁）。
     */
    m_thread->join();
}

void EvThread::Start()
{
    /**
     * 非线程安全；Running 时直接返回。Finish 不代表句柄已回收：自然结束时旧句柄仍在，Start 不会新建线程。
     */
    if (m_status == Running)
        return;

    StartWorkFunc();
}

core::errcode::ErrOpt EvThread::Stop()
{
    /**
     * 停止序列：BreakLoop → join → delete 线程对象；未 Start() 时 m_thread 为空，见头文件前置条件。
     */
    if (m_status == Finish)
        return std::nullopt;

    auto err = m_eventloop->BreakLoop();
    if (err != 0)
        return core::errcode::Errcode{"stop failed!", emErr::ERR_UNKNOWN};

    /**
     * 阻塞式的等待
     */
    SyncWaitThreadExit();
    Destory();

    return std::nullopt;
}

void EvThread::WorkHandle()
{
    /**
     * 循环到 BreakLoop；停止标记生效时 StartLoop 返回 1，与下方 Assert 期望的 0 不同。
     */
    auto err = m_eventloop->StartLoop(EVLOOP_NO_EXIT_ON_EMPTY);
    Assert(err == 0);
}

bool EvThread::SyncWaitThreadExitEx(int wait_time)
{
    /**
     * wait_time < 0 才真正 join；> 0 只按 interval 轮询 joinable，线程仍在，恒返回 false；== 0 直接返回 false。
     */

    /**
     * 休眠间隔
     */
    const int interval = 50;

    if (wait_time == 0)
        return false;
    int increase = wait_time > 0 ? interval : 0;
    int pass_time = 0;

    if (wait_time < 0 && m_thread->joinable())
        m_thread->join();

    while (m_thread->joinable() && pass_time < wait_time) {
        std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        pass_time += increase;
    }

    return !(m_thread->joinable());
}

void EvThread::SyncWaitThreadExit()
{
    SyncWaitThreadExitEx(-1);
}

bool EvThread::SyncWaitThreadExitWithTime(int wait_time)
{
    return SyncWaitThreadExitEx(wait_time);
}

}
