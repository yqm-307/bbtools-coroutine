#include <atomic>
#include <bbt/pollevent/EvThread.hpp>
#include <bbt/pollevent/EventLoop.hpp>


using namespace bbt::pollevent::detail;

namespace bbt::pollevent
{

static const int io_thread_limit_num = 16;

EvThreadId EvThread::GenerateTid()
{
    static std::atomic_int _id = 0;
    return ++_id;
}


EvThread::EvThread(std::shared_ptr<EventLoop> evloop):
    m_eventloop(evloop),
    m_tid(GenerateTid())
{
}

EvThread::EvThread():
    m_eventloop(std::make_shared<EventLoop>()),
    m_tid(GenerateTid())
{
}

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
    return m_thread->get_id();
}

void EvThread::StartWorkFunc()
{
    if (m_thread != nullptr)
        return;

    m_thread = new std::thread([=](){
        Work();
    });
}

void EvThread::Work()
{
    m_status = EvThreadRunStatus::Running;
    WorkHandle();
    m_status = EvThreadRunStatus::Finish;
}

bool EvThread::IsRunning() const
{
    return (m_status == EvThreadRunStatus::Running);
}

std::shared_ptr<Event> EvThread::RegisterEvent(evutil_socket_t fd, short events, const bbt::pollevent::OnEventCallback& onevent_cb)
{
    auto event_sptr = m_eventloop->CreateEvent(fd, events, onevent_cb);
    return event_sptr;
}

void EvThread::Join()
{
    m_thread->join();
}


void EvThread::Start()
{
    if (m_status == Running)
        return;

    StartWorkFunc();
}

core::errcode::ErrOpt EvThread::Stop()
{
    if (m_status == Finish)
        return std::nullopt;

    auto err = m_eventloop->BreakLoop();
    if (err != 0)
        return core::errcode::Errcode{"stop failed!", emErr::ERR_UNKNOWN};

    /* 阻塞式的等待 */
    SyncWaitThreadExit();
    Destory();

    return std::nullopt;
}

void EvThread::WorkHandle()
{
    auto err = m_eventloop->StartLoop(EVLOOP_NO_EXIT_ON_EMPTY);
    Assert(err == 0);
}

bool EvThread::SyncWaitThreadExitEx(int wait_time)
{
    const int interval = 50;    // 休眠间隔
    if(wait_time == 0)
        return false;
    int increase = wait_time > 0 ? interval : 0;
    int pass_time = 0;

    if(wait_time < 0 && m_thread->joinable())
        m_thread->join();
    
    while (m_thread->joinable() && pass_time < wait_time)
    {
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



} // namespace bbt::pollevent