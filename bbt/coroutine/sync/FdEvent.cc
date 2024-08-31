#include <bbt/coroutine/sync/FdEvent.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

namespace bbt::coroutine::sync
{

std::shared_ptr<FdEvent> FdEvent::Create()
{
    return std::shared_ptr<FdEvent>();
}


FdEvent::FdEvent()
{

}

FdEvent::~FdEvent()
{

}

int FdEvent::WaitUntilReadable(int fd, int ms)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    auto [ret, id] = g_bbt_poller->RegistFdReadable<FdEvent>(fd, ms);

    if (ret != 0)
        return ret;

    auto co = GetWaitCo();
    ret = co->YieldWithCallback([&lock](){
        lock.unlock();
        return true;
    });

    return ret;
}

int FdEvent::WaitUntilWriteable(int fd, int ms)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    auto [ret, id] = g_bbt_poller->RegistFdWriteable<FdEvent>(fd, ms);

    if (ret != 0) return ret;

    auto co = GetWaitCo();
    ret = co->YieldWithCallback([&lock](){
        lock.unlock();
        return true;
    });

    return ret;
}

int FdEvent::Trigger(short trigger_events, int customkey)
{
    std::lock_guard<std::mutex> _(m_mutex);
    auto wait_co = GetWaitCo();
    Assert(wait_co != nullptr);

    g_scheduler->OnActiveCoroutine(wait_co);

    return 0;
}

}