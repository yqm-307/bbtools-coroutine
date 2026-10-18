#include <bbt/coroutine/detail/DriverWakeDeferral.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>

namespace bbt::coroutine::detail
{

int& DriverWakeDeferral::_TlsDepth() noexcept
{
    static thread_local int tl_depth = 0;
    return tl_depth;
}

bool& DriverWakeDeferral::_TlsPending() noexcept
{
    static thread_local bool tl_pending = false;
    return tl_pending;
}

bool DriverWakeDeferral::Defer() noexcept
{
    if (_TlsDepth() <= 0)
        return false;

    _TlsPending() = true;
    return true;
}

DriverWakeDeferral::~DriverWakeDeferral() noexcept
{
    if (--_TlsDepth() > 0)
        return;

    _TlsDepth() = 0;    // 防下溢（异常配对破坏时归位）
    if (!_TlsPending())
        return;

    _TlsPending() = false;
    Scheduler::_WakeProcesserDrivers();
}

}
