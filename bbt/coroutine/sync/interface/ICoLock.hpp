/** @file ICoLock.hpp @brief 协程锁抽象接口。 */
#pragma once

namespace bbt::coroutine::sync {

/**
 * @brief 协程锁抽象接口；Lock 被占用时挂起当前协程而不是阻塞 worker 线程。
 */
class ICoLock {
public:
    /// @brief 加锁；被占用时挂起当前协程，只能在协程内调用。
    virtual void Lock() = 0;
    /// @brief 解锁；不挂起，可在非协程上下文调用。
    virtual void UnLock() = 0;
    /// @brief 立即尝试加锁，不挂起；0 成功，-1 未获得。
    virtual int TryLock() = 0;
    /// @brief 在 ms 毫秒内尝试加锁；0 成功，1 超时，-1 失败（不能立即获锁且 ms<0）。
    virtual int TryLock(int ms) = 0;
};

} // namespace bbt::coroutine::sync
