/**
 * @file CoLockGuard.hpp
 * @brief 协程锁的 RAII 守卫：CoLockGuard、CoUniqueLock 与读写锁守卫。
 */
#pragma once
#include <bbt/core/util/Assert.hpp>
#include <bbt/coroutine/sync/CoRWMutex.hpp>
#include <memory>
#include <system_error>
#include <utility>

namespace bbt::coroutine::sync {

/**
 * @brief RAII 协程锁守卫：构造即 Lock()、析构 UnLock()，持 shared_ptr 保锁生命周期。
 *        构造须在协程内（Lock 会挂起）；析构 noexcept；死锁检测靠底层 Mutex::Lock() 的断言。
 */
template <typename Mutex> class CoLockGuard {
public:
    /**
     * @brief 构造即调用 Lock()；Lock() 抛出的异常直接传给调用方。
     */
    explicit CoLockGuard(const std::shared_ptr<Mutex>& m) noexcept(false);

    /**
     * @brief 析构时调用 UnLock()，noexcept。
     */
    ~CoLockGuard() noexcept;

    CoLockGuard(const CoLockGuard&) = delete;
    CoLockGuard& operator=(const CoLockGuard&) = delete;

private:
    std::shared_ptr<Mutex> m_mutex;
};

/**
 * @brief RAII 协程锁：defer_lock / try_to_lock / adopt_lock / try_lock_for(ms) / 移动，持 shared_ptr 保锁生命周期。
 *        构造与 lock() 会在协程内挂起；try_lock_for 把超时和异常都折算成 false；析构 noexcept。
 */
template <typename Mutex> class CoUniqueLock {
public:
    /**
     * @brief 构造一个不关联锁的空守卫。
     */
    CoUniqueLock() noexcept;

    /**
     * @brief 构造即加锁；Lock() 抛出的异常直接传给调用方。
     */
    explicit CoUniqueLock(const std::shared_ptr<Mutex>& m) noexcept(false);

    /**
     * @brief 只关联锁，不立即加锁。
     */
    CoUniqueLock(const std::shared_ptr<Mutex>& m, std::defer_lock_t) noexcept;

    /**
     * @brief 构造时 TryLock() 一次，失败则保持未持锁。
     */
    CoUniqueLock(const std::shared_ptr<Mutex>& m, std::try_to_lock_t) noexcept;

    /**
     * @brief 接管调用方已持有的锁，不再加锁。
     */
    CoUniqueLock(const std::shared_ptr<Mutex>& m, std::adopt_lock_t) noexcept;

    /**
     * @brief 移动构造：锁所有权转给新对象，源对象变为未持锁。
     */
    CoUniqueLock(CoUniqueLock&& other) noexcept;

    /**
     * @brief 移动赋值：先释放自己持有的锁，再接管源对象的锁。
     */
    CoUniqueLock& operator=(CoUniqueLock&& other) noexcept;

    /**
     * @brief 析构：持锁时调用 UnLock()，noexcept。
     */
    ~CoUniqueLock() noexcept;

    CoUniqueLock(const CoUniqueLock&) = delete;
    CoUniqueLock& operator=(const CoUniqueLock&) = delete;

    /**
     * @brief 加锁；未关联锁或已持锁时断言。
     */
    void lock() noexcept(false);

    /**
     * @brief TryLock() 一次；返回是否拿到锁。
     */
    bool try_lock() noexcept;

    /**
     * @brief 在 ms 毫秒内 TryLock(ms)；超时与异常都折算成 false。
     */
    bool try_lock_for(int ms) noexcept;

    /**
     * @brief 释放锁；未持锁时断言。
     */
    void unlock() noexcept;

    /**
     * @brief 是否持有锁。
     */
    bool owns_lock() const noexcept;

    /**
     * @brief 同 owns_lock()。
     */
    explicit operator bool() const noexcept;

    /**
     * @brief 取出关联的锁对象（可能为空）。
     */
    std::shared_ptr<Mutex> mutex() const noexcept;

private:
    std::shared_ptr<Mutex> m_mutex;
    bool m_owns;
};

/**
 * @name CoRWMutex 专用 RAII 守卫
 * @{
 */

/**
 * @brief RAII 读锁守卫：构造 RLock()、析构 RUnLock()（noexcept），支持 defer/try/adopt/try_lock_for 与移动。
 * @note 不支持读锁升级：持读锁再取写锁会互相等待，CoRWMutex 不检测，必须避免。
 */
class CoReadLock {
public:
    using SPtr = std::shared_ptr<CoRWMutex>;

    /**
     * @brief 构造一个不关联锁的空守卫。
     */
    CoReadLock() noexcept;

    /**
     * @brief 构造即 RLock()；失败抛 std::system_error，不置 owns。
     */
    explicit CoReadLock(const SPtr& m) noexcept(false);

    /**
     * @brief 只关联锁，不立即加锁。
     */
    CoReadLock(const SPtr& m, std::defer_lock_t) noexcept;

    /**
     * @brief 构造时 TryRLock() 一次，失败则保持未持锁。
     */
    CoReadLock(const SPtr& m, std::try_to_lock_t) noexcept;

    /**
     * @brief 接管调用方已持有的读锁，不再加锁。
     */
    CoReadLock(const SPtr& m, std::adopt_lock_t) noexcept;

    /**
     * @brief 移动构造：读锁所有权转给新对象，源对象变为未持锁。
     */
    CoReadLock(CoReadLock&& other) noexcept;

    /**
     * @brief 移动赋值：先释放自己持有的读锁，再接管源对象的读锁。
     */
    CoReadLock& operator=(CoReadLock&& other) noexcept;

    /**
     * @brief 析构：持读锁时调用 RUnLock()，noexcept。
     */
    ~CoReadLock() noexcept;

    CoReadLock(const CoReadLock&) = delete;
    CoReadLock& operator=(const CoReadLock&) = delete;

    /**
     * @brief 获取读锁；失败抛 std::system_error。
     */
    void lock() noexcept(false);

    /**
     * @brief TryRLock() 一次；返回是否拿到读锁。
     */
    bool try_lock() noexcept;

    /**
     * @brief 在 ms 毫秒内 TryRLock(ms)；异常、超时、被抢先都折算成 false。
     */
    bool try_lock_for(int ms) noexcept;

    /**
     * @brief 释放读锁；未持锁时断言。
     */
    void unlock() noexcept;

    /**
     * @brief 是否持有读锁。
     */
    bool owns_lock() const noexcept;

    /**
     * @brief 同 owns_lock()。
     */
    explicit operator bool() const noexcept;

    /**
     * @brief 取出关联的读写锁（可能为空）。
     */
    SPtr mutex() const noexcept;

private:
    SPtr m_mutex;
    bool m_owns;
};

/**
 * @brief RAII 写锁守卫：构造 WLock()、析构 WUnLock()（noexcept），支持 defer/try/adopt/try_lock_for 与移动。
 * @note 不支持读锁升级：持读锁再取写锁会互相等待，CoRWMutex 不检测，必须避免。
 */
class CoWriteLock {
public:
    using SPtr = std::shared_ptr<CoRWMutex>;

    /**
     * @brief 构造一个不关联锁的空守卫。
     */
    CoWriteLock() noexcept;

    /**
     * @brief 构造即 WLock()；失败抛 std::system_error，不置 owns。
     */
    explicit CoWriteLock(const SPtr& m) noexcept(false);

    /**
     * @brief 只关联锁，不立即加锁。
     */
    CoWriteLock(const SPtr& m, std::defer_lock_t) noexcept;

    /**
     * @brief 构造时 TryWLock() 一次，失败则保持未持锁。
     */
    CoWriteLock(const SPtr& m, std::try_to_lock_t) noexcept;

    /**
     * @brief 接管调用方已持有的写锁，不再加锁。
     */
    CoWriteLock(const SPtr& m, std::adopt_lock_t) noexcept;

    /**
     * @brief 移动构造：写锁所有权转给新对象，源对象变为未持锁。
     */
    CoWriteLock(CoWriteLock&& other) noexcept;

    /**
     * @brief 移动赋值：先释放自己持有的写锁，再接管源对象的写锁。
     */
    CoWriteLock& operator=(CoWriteLock&& other) noexcept;

    /**
     * @brief 析构：持写锁时调用 WUnLock()，noexcept。
     */
    ~CoWriteLock() noexcept;

    CoWriteLock(const CoWriteLock&) = delete;
    CoWriteLock& operator=(const CoWriteLock&) = delete;

    /**
     * @brief 获取写锁；失败抛 std::system_error。
     */
    void lock() noexcept(false);

    /**
     * @brief TryWLock() 一次；返回是否拿到写锁。
     */
    bool try_lock() noexcept;

    /**
     * @brief 在 ms 毫秒内 TryWLock(ms)；异常、超时、被抢先都折算成 false。
     */
    bool try_lock_for(int ms) noexcept;

    /**
     * @brief 释放写锁；未持锁时断言。
     */
    void unlock() noexcept;

    /**
     * @brief 是否持有写锁。
     */
    bool owns_lock() const noexcept;

    /**
     * @brief 同 owns_lock()。
     */
    explicit operator bool() const noexcept;

    /**
     * @brief 取出关联的读写锁（可能为空）。
     */
    SPtr mutex() const noexcept;

private:
    SPtr m_mutex;
    bool m_owns;
};

/**
 * @}
 */

}

#include <bbt/coroutine/sync/__TCoLockGuard.hpp>
