#include <unistd.h>
#include <fcntl.h>
#include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>

namespace bbt::coroutine::sync
{

using namespace bbt::coroutine::detail;

CoCond::UPtr&& CoCond::Create()
{
    auto cond = std::make_unique<CoCond>();
    int ret =  cond->Init();
    return (ret == 0 ? std::move(cond) : nullptr);
}


CoCond::CoCond()
{

}

CoCond::~CoCond()
{

}

int CoCond::Init()
{
    int ret = ::pipe(m_pipe_fds);
    if (ret != 0) return ret;

    ret = ::fcntl(m_pipe_fds[1], F_SETFL, O_NONBLOCK);
    if (ret != 0) return ret;

    auto co = g_bbt_coroutine_co;
    if (co == nullptr)
        return -1;

    m_poller_event = CoPollEvent::Create(co, m_pipe_fds[0], 10, [](Coroutine::SPtr co){
        co->OnEventReadable();
    });

    return 0;
}

int CoCond::Wait()
{
    char byte;
    auto co = g_bbt_coroutine_co;
    if (co == nullptr)
        return -1;

    if (m_poller_event->RegistEvent() != 0)
        return -1;

    co->Yield();

    /*XXX 可能读不尽*/
    if (::read(m_pipe_fds[0], &byte, 1) < 0)
        return -1;

    return 0;
}

int CoCond::Notify()
{
    size_t n = ::write(m_pipe_fds[1], (void*)'y', 1);
    if (n < 0) return -1;

    return 0;
}

}