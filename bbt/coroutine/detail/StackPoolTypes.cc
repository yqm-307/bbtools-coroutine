#include <bbt/coroutine/detail/StackPoolTypes.hpp>
#include <bbt/coroutine/detail/StackPool.hpp>

namespace bbt::coroutine::detail {

void StackReturner::operator()(Stack* stack) const noexcept
{
    if (stack != nullptr)
        pool->Release(stack);
}

StackWaitError::StackWaitError(WaitStatus status)
    : std::runtime_error("coroutine stack acquisition interrupted"), m_status(status)
{
}

WaitStatus StackWaitError::Status() const noexcept
{
    return m_status;
}

}
