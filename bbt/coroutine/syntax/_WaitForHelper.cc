#include <bbt/coroutine/syntax/_WaitForHelper.hpp>
#include <bbt/core/util/Assert.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>

namespace bbt::coroutine {

/* 挂起发生在构造里：先确认当前在协程上下文（否则断言），再让出到 fd 事件。
 * YieldUntilFdEx 只在登记/挂起失败（-1、CHECK_FAILED）或等待被取消（1）时返回非 0，
 * 超时是正常唤醒、返回 0；非 0 在这里统一转成异常，因为宏没有返回码出口。 */
_WaitForHelper::_WaitForHelper(int fd, short event, int ms)
{
    AssertWithInfo(g_bbt_tls_coroutine_co != nullptr, "must be in coroutine context");

    if (g_bbt_tls_coroutine_co->YieldUntilFdEx(fd, event, ms) != 0)
        throw std::runtime_error("wait for failed!");
}

}
