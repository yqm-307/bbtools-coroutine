#pragma once
#include <future>
#include <memory>
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::pool
{

class Work
{
public:
    Work(CoPoolWorkCallback&& workfunc):m_callback(std::move(workfunc)) {}
    Work(const CoPoolWorkCallback& workfunc):m_callback(workfunc) {}

    /**
     * @brief 带结果传递的任务：SubmitWithFuture 使用。
     *        任务异常通过 promise 交付给调用方 future，不参与池级异常策略。
     */
    Work(CoPoolWorkCallback&& workfunc, std::shared_ptr<std::promise<void>> promise):
        m_callback(std::move(workfunc)),
        m_promise(std::move(promise)) {}

    Work(const CoPoolWorkCallback& workfunc, std::shared_ptr<std::promise<void>> promise):
        m_callback(workfunc),
        m_promise(std::move(promise)) {}

    virtual ~Work() {}

    virtual void Invoke()
    {
        m_callback();
    }

    bool HasPromise() const noexcept
    {
        return (m_promise != nullptr);
    }

    /* 任务异常交付：future 路径 set_exception，未取 future 时抛回 */
    void SetException(const std::exception_ptr& eptr) noexcept
    {
        if (m_promise != nullptr)
            m_promise->set_exception(eptr);
    }

    /* 任务正常完成交付（SubmitWithFuture 路径） */
    void SetFinished() noexcept
    {
        if (m_promise != nullptr)
            m_promise->set_value();
    }

private:
    CoPoolWorkCallback m_callback{nullptr};
    std::shared_ptr<std::promise<void>> m_promise{nullptr};
};

} // namespace bbt::coroutine::pool
