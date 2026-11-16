/**
 * @file Work.cc
 * @brief Work 的成员实现，头文件只保留声明。
 */
#include <bbt/coroutine/pool/Work.hpp>

namespace bbt::coroutine::pool {

Work::Work(CoPoolWorkCallback&& workfunc) : m_callback(std::move(workfunc)) {}

Work::Work(const CoPoolWorkCallback& workfunc) : m_callback(workfunc) {}

Work::Work(CoPoolWorkCallback&& workfunc, std::shared_ptr<std::promise<void>> promise)
    : m_callback(std::move(workfunc)), m_promise(std::move(promise))
{
}

Work::Work(const CoPoolWorkCallback& workfunc, std::shared_ptr<std::promise<void>> promise)
    : m_callback(workfunc), m_promise(std::move(promise))
{
}

Work::~Work() {}

void Work::Invoke()
{
    m_callback();
}

bool Work::HasPromise() const noexcept
{
    return (m_promise != nullptr);
}

void Work::SetException(const std::exception_ptr& eptr) noexcept
{
    if (m_promise != nullptr)
        m_promise->set_exception(eptr);
}

void Work::SetFinished() noexcept
{
    if (m_promise != nullptr)
        m_promise->set_value();
}

}
