#include <bbt/coroutine/syntax/_WaitForHelper.hpp>
#include <bbt/core/util/Assert.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/WaitProtocol.hpp>

namespace bbt::coroutine
{

_WaitForHelper::_WaitForHelper(int fd, short event, int ms)
{
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "must be in coroutine context");

    if (detail::WaitProtocolBridge::WaitFd(*g_bbt_tls_coroutine_co, fd, event, ms)
        == detail::WaitResult::WAIT_ERROR)
        throw std::runtime_error("wait for failed!");
}

}