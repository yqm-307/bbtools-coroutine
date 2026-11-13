/**
 * @file StdLockWapper.hpp
 * @brief 把 ICoLock 适配成 std 锁概念。
 */
#pragma once
#include <memory>
#include <bbt/coroutine/sync/interface/ICoLock.hpp>

namespace bbt::coroutine::sync {

/**
 * @brief ICoLock 的 std 锁概念适配器，供 std::lock_guard/std::unique_lock 包住协程锁。
 * lock() 被占用时挂起当前协程而非阻塞 worker，故只能在协程内用；try_lock() 要求实现不挂起。
 */
class StdLockWapper {
public:
    /**
     * @brief 包装一个已有 ICoLock，与调用方共享其所有权。
     */
    StdLockWapper(std::shared_ptr<ICoLock> colock) : m_co_lock(colock) {};

    /**
     * @brief 空析构：不解锁，锁的生命期由 shared_ptr 决定。
     */
    ~StdLockWapper() {}

    /**
     * @brief 加锁；锁被占用时挂起当前协程。
     */
    void lock() { m_co_lock->Lock(); }

    /**
     * @brief 解锁。
     */
    void unlock() { m_co_lock->UnLock(); }

    /**
     * @brief 立即尝试加锁，不挂起；成功返回 true。
     */
    bool try_lock() { return (m_co_lock->TryLock() == 0); }

private:
    std::shared_ptr<ICoLock> m_co_lock{nullptr};
};

}