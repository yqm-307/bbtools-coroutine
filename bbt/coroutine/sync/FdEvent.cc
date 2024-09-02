#include <bbt/coroutine/sync/FdEvent.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

namespace bbt::coroutine::sync
{

std::shared_ptr<FdEvent> FdEvent::Create()
{
    return std::make_shared<FdEvent>();
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
    auto [ret, id] = g_bbt_poller->RegistFdReadable<FdEvent>(shared_from_this(), fd, ms);

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
    auto [ret, id] = g_bbt_poller->RegistFdWriteable<FdEvent>(shared_from_this(), fd, ms);

    if (ret != 0) return ret;

    auto co = GetWaitCo();
    ret = co->YieldWithCallback([&lock](){
        lock.unlock();
        return true;
    });

    return ret;
}

int FdEvent::OnNotify(short trigger_events, int customkey)
{
    std::lock_guard<std::mutex> _(m_mutex);
    auto wait_co = GetWaitCo();
    Assert(wait_co != nullptr);

    wait_co->Active();

    return 0;
}

}