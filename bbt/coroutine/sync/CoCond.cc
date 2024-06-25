#include <unistd.h>
#include <fcntl.h>
#include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>

namespace bbt::coroutine::sync
{

using namespace bbt::coroutine::detail;

CoCond::SPtr CoCond::Create()
{
    auto cond = std::make_shared<CoCond>();
    int ret =  cond->Init();
    return (ret == 0 ? std::move(cond) : nullptr);
}


CoCond::CoCond()
{

}

CoCond::~CoCond()
{
    ::close(m_pipe_fds[0]);
    ::close(m_pipe_fds[1]);
}

int CoCond::Init()
{
    int ret = ::pipe(m_pipe_fds);
    if (ret != 0) return ret;

    ret = ::fcntl(m_pipe_fds[1], F_SETFL, O_NONBLOCK);
    ret = ::fcntl(m_pipe_fds[0], F_SETFL ,O_NONBLOCK);
    if (ret != 0) return ret;

    /* 只能在正在运行的协程上创建cond */
    auto co = g_bbt_tls_coroutine_co;
    if (co == nullptr)
        return -1;

    return 0;
}

int CoCond::Wait()
{
    int ret = 0;
    char byte[32];
    auto co = g_bbt_tls_coroutine_co;
    if (co == nullptr)
        return -1;

    if (co->RegistReadableET(m_pipe_fds[0], 10) == nullptr)
        return -1;

    co->Yield();

    /*XXX 可能读不尽，如果调用Notify次数过多有问题，先拓展byte大小，尽量容许Notify重复多次 */
    if (::read(m_pipe_fds[0], &byte, sizeof(byte)) < 0)
        return -1;

    return 0;
}

int CoCond::WaitWithTimeout(int ms)
{
    Assert(ms > 0);
    char byte;
    auto co = g_bbt_tls_coroutine_co;
    if (co == nullptr)
        return -1;

    if (co->RegistReadableET(m_pipe_fds[0], 10) == nullptr)
        return -1;

    /* 注册超时事件 */
    if (co->RegistTimeout(ms) == nullptr)
        return -1;

    /* 协程挂起 */
    co->Yield();

    /*XXX 可能读不尽*/
    if (::read(m_pipe_fds[0], &byte, 1) < 0)
        return -1;

    return 0;
}

int CoCond::Notify()
{
    size_t n = ::write(m_pipe_fds[1], (void*)"y", 1);
    if (n < 0) return -1;

    return 0;
}

}