/** @file Work.hpp @brief 池内任务载体：回调 + 可选的 future 交付通道，所有权随入队交给 CoPool。 */
#pragma once
#include <future>
#include <memory>
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::pool {

/**
 * @brief 池任务：持有回调，带 promise 时负责把结果交付给 future。Submit* 里 new、入队即交给池，执行完
 *        立刻 delete；池停止时队列剩余的也被 delete，未兑现的 future 得到 broken_promise 而非永挂。
 */
class Work {
public:
    /// @brief 移动回调构造；异常走池级策略（回调或计数），不产生 future
    Work(CoPoolWorkCallback&& workfunc) : m_callback(std::move(workfunc))
    {
    }
    /// @brief 拷贝回调构造
    Work(const CoPoolWorkCallback& workfunc) : m_callback(workfunc)
    {
    }

    /**
     * @brief 带结果传递的任务：SubmitWithFuture 使用。
     *        任务异常通过 promise 交付给调用方 future，不参与池级异常策略。
     */
    Work(CoPoolWorkCallback&& workfunc, std::shared_ptr<std::promise<void>> promise)
        : m_callback(std::move(workfunc)), m_promise(std::move(promise))
    {
    }

    /// @brief 同上（拷贝回调）
    Work(const CoPoolWorkCallback& workfunc, std::shared_ptr<std::promise<void>> promise)
        : m_callback(workfunc), m_promise(std::move(promise))
    {
    }

    /// @brief 析构时未兑现的 promise 会以 broken_promise 交付给等待中的 future
    virtual ~Work()
    {
    }

    /// @brief 执行回调；异常不在这里处理，由调用方（work 协程）捕获后交给 _HandleWorkException
    virtual void Invoke()
    {
        m_callback();
    }

    /// @brief 是否带 promise；决定异常走 future 还是池级策略，_HandleWorkException 先问它
    bool HasPromise() const noexcept
    {
        return (m_promise != nullptr);
    }

    /// @brief 任务异常交付：只对带 promise 的任务生效，其余交给池级策略，所以这里不抛
    void SetException(const std::exception_ptr& eptr) noexcept
    {
        if (m_promise != nullptr)
            m_promise->set_exception(eptr);
    }

    /// @brief 任务正常完成交付（SubmitWithFuture 路径）；不带 promise 时是空操作
    void SetFinished() noexcept
    {
        if (m_promise != nullptr)
            m_promise->set_value();
    }

private:
    CoPoolWorkCallback m_callback{nullptr};
    /* 与 Work 同生命期：Work 被 delete 时释放，未兑现的 future 因此拿到 broken_promise */
    std::shared_ptr<std::promise<void>> m_promise{nullptr};
};

} // namespace bbt::coroutine::pool
