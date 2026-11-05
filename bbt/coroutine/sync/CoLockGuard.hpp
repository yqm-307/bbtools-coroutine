/** @file CoLockGuard.hpp
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
    /// @brief 构造即调用 Lock()；Lock() 抛出的异常直接传给调用方。
    explicit CoLockGuard(const std::shared_ptr<Mutex>& m) noexcept(false) : m_mutex(m)
    {
        m_mutex->Lock();
    }

    /// @brief 析构时调用 UnLock()，noexcept。
    ~CoLockGuard() noexcept
    {
        if (m_mutex)
            m_mutex->UnLock();
    }

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
    /// @brief 构造一个不关联锁的空守卫。
    CoUniqueLock() noexcept : m_mutex(nullptr), m_owns(false)
    {
    }

    /// @brief 构造即加锁；Lock() 抛出的异常直接传给调用方。
    explicit CoUniqueLock(const std::shared_ptr<Mutex>& m) noexcept(false) : m_mutex(m), m_owns(false)
    {
        m_mutex->Lock();
        m_owns = true;
    }

    /// @brief 只关联锁，不立即加锁。
    CoUniqueLock(const std::shared_ptr<Mutex>& m, std::defer_lock_t) noexcept : m_mutex(m), m_owns(false)
    {
    }

    /// @brief 构造时 TryLock() 一次，失败则保持未持锁。
    CoUniqueLock(const std::shared_ptr<Mutex>& m, std::try_to_lock_t) noexcept : m_mutex(m), m_owns(false)
    {
        m_owns = (m_mutex->TryLock() == 0);
    }

    /// @brief 接管调用方已持有的锁，不再加锁。
    CoUniqueLock(const std::shared_ptr<Mutex>& m, std::adopt_lock_t) noexcept : m_mutex(m), m_owns(true)
    {
    }

    /// @brief 移动构造：锁所有权转给新对象，源对象变为未持锁。
    CoUniqueLock(CoUniqueLock&& other) noexcept : m_mutex(std::move(other.m_mutex)), m_owns(other.m_owns)
    {
        other.m_owns = false;
    }

    /// @brief 移动赋值：先释放自己持有的锁，再接管源对象的锁。
    CoUniqueLock& operator=(CoUniqueLock&& other) noexcept
    {
        if (m_owns)
            m_mutex->UnLock();
        m_mutex = std::move(other.m_mutex);
        m_owns = other.m_owns;
        other.m_owns = false;
        return *this;
    }

    /// @brief 析构：持锁时调用 UnLock()，noexcept。
    ~CoUniqueLock() noexcept
    {
        if (m_owns && m_mutex)
            m_mutex->UnLock();
    }

    CoUniqueLock(const CoUniqueLock&) = delete;
    CoUniqueLock& operator=(const CoUniqueLock&) = delete;

    /// @brief 加锁；未关联锁或已持锁时断言。
    void lock() noexcept(false)
    {
        AssertWithInfo(m_mutex != nullptr, "CoUniqueLock::lock(): no associated mutex");
        AssertWithInfo(!m_owns, "CoUniqueLock::lock(): already owns the lock (deadlock would occur)");
        m_mutex->Lock();
        m_owns = true;
    }

    /// @brief TryLock() 一次；返回是否拿到锁。
    bool try_lock() noexcept
    {
        AssertWithInfo(m_mutex != nullptr, "CoUniqueLock::try_lock(): no associated mutex");
        AssertWithInfo(!m_owns, "CoUniqueLock::try_lock(): already owns the lock");
        m_owns = (m_mutex->TryLock() == 0);
        return m_owns;
    }

    /// @brief 在 ms 毫秒内 TryLock(ms)；超时与异常都折算成 false。
    bool try_lock_for(int ms) noexcept
    {
        AssertWithInfo(m_mutex != nullptr, "CoUniqueLock::try_lock_for(): no associated mutex");
        AssertWithInfo(!m_owns, "CoUniqueLock::try_lock_for(): already owns the lock");
        /* 等待准备可能抛出（如分配失败）；noexcept 内收敛为未获锁，不 terminate。 */
        try {
            m_owns = (m_mutex->TryLock(ms) == 0);
        } catch (...) {
            m_owns = false;
        }
        return m_owns;
    }

    /// @brief 释放锁；未持锁时断言。
    void unlock() noexcept
    {
        AssertWithInfo(m_mutex != nullptr, "CoUniqueLock::unlock(): no associated mutex");
        AssertWithInfo(m_owns, "CoUniqueLock::unlock(): does not own the lock");
        m_mutex->UnLock();
        m_owns = false;
    }

    /// @brief 是否持有锁。
    bool owns_lock() const noexcept
    {
        return m_owns;
    }
    /// @brief 同 owns_lock()。
    explicit operator bool() const noexcept
    {
        return m_owns;
    }
    /// @brief 取出关联的锁对象（可能为空）。
    std::shared_ptr<Mutex> mutex() const noexcept
    {
        return m_mutex;
    }

private:
    std::shared_ptr<Mutex> m_mutex;
    bool m_owns;
};

// ============================================================================
// CoRWMutex 专用 RAII 守卫
// ============================================================================

/**
 * @brief RAII 读锁守卫：构造 RLock()、析构 RUnLock()（noexcept），支持 defer/try/adopt/try_lock_for 与移动。
 * @note 不支持读锁升级：持读锁再取写锁会互相等待，CoRWMutex 不检测，必须避免。
 */
class CoReadLock {
public:
    using SPtr = std::shared_ptr<CoRWMutex>;

    /// @brief 构造一个不关联锁的空守卫。
    CoReadLock() noexcept : m_mutex(nullptr), m_owns(false)
    {
    }

    /// @brief 构造即 RLock()；失败抛 std::system_error，不置 owns。
    explicit CoReadLock(const SPtr& m) noexcept(false) : m_mutex(m), m_owns(false)
    {
        /* RLock 返回 -1 时不得静默置 owns（无锁进临界区 / 析构误归还别人的锁），按 unique_lock 惯例抛 system_error。 */
        if (m_mutex->RLock() != 0)
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                    "CoReadLock: RLock failed");
        m_owns = true;
    }

    /// @brief 只关联锁，不立即加锁。
    CoReadLock(const SPtr& m, std::defer_lock_t) noexcept : m_mutex(m), m_owns(false)
    {
    }

    /// @brief 构造时 TryRLock() 一次，失败则保持未持锁。
    CoReadLock(const SPtr& m, std::try_to_lock_t) noexcept : m_mutex(m), m_owns(false)
    {
        m_owns = (m_mutex->TryRLock() == 0);
    }

    /// @brief 接管调用方已持有的读锁，不再加锁。
    CoReadLock(const SPtr& m, std::adopt_lock_t) noexcept : m_mutex(m), m_owns(true)
    {
    }

    /// @brief 移动构造：读锁所有权转给新对象，源对象变为未持锁。
    CoReadLock(CoReadLock&& other) noexcept : m_mutex(std::move(other.m_mutex)), m_owns(other.m_owns)
    {
        other.m_owns = false;
    }

    /// @brief 移动赋值：先释放自己持有的读锁，再接管源对象的读锁。
    CoReadLock& operator=(CoReadLock&& other) noexcept
    {
        if (m_owns)
            m_mutex->RUnLock();
        m_mutex = std::move(other.m_mutex);
        m_owns = other.m_owns;
        other.m_owns = false;
        return *this;
    }

    /// @brief 析构：持读锁时调用 RUnLock()，noexcept。
    ~CoReadLock() noexcept
    {
        if (m_owns && m_mutex)
            m_mutex->RUnLock();
    }

    CoReadLock(const CoReadLock&) = delete;
    CoReadLock& operator=(const CoReadLock&) = delete;

    /// @brief 获取读锁；失败抛 std::system_error。
    void lock() noexcept(false)
    {
        AssertWithInfo(m_mutex != nullptr, "CoReadLock::lock(): no associated mutex");
        AssertWithInfo(!m_owns, "CoReadLock::lock(): already owns the read lock");
        /* 同构造路径——登记失败以 system_error 抛出，不置 owns。 */
        if (m_mutex->RLock() != 0)
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                    "CoReadLock::lock(): RLock failed");
        m_owns = true;
    }

    /// @brief TryRLock() 一次；返回是否拿到读锁。
    bool try_lock() noexcept
    {
        AssertWithInfo(m_mutex != nullptr, "CoReadLock::try_lock(): no associated mutex");
        AssertWithInfo(!m_owns, "CoReadLock::try_lock(): already owns the read lock");
        m_owns = (m_mutex->TryRLock() == 0);
        return m_owns;
    }

    /// @brief 在 ms 毫秒内 TryRLock(ms)；异常、超时、被抢先都折算成 false。
    bool try_lock_for(int ms) noexcept
    {
        AssertWithInfo(m_mutex != nullptr, "CoReadLock::try_lock_for(): no associated mutex");
        AssertWithInfo(!m_owns, "CoReadLock::try_lock_for(): already owns the read lock");
        /* 等待准备可能抛出（如分配失败）；noexcept 内收敛为 false。错误与超时在 bool 通道上不可分辨。 */
        try {
            m_owns = (m_mutex->TryRLock(ms) == 0);
        } catch (...) {
            m_owns = false;
        }
        return m_owns;
    }

    /// @brief 释放读锁；未持锁时断言。
    void unlock() noexcept
    {
        AssertWithInfo(m_mutex != nullptr, "CoReadLock::unlock(): no associated mutex");
        AssertWithInfo(m_owns, "CoReadLock::unlock(): does not own the read lock");
        m_mutex->RUnLock();
        m_owns = false;
    }

    /// @brief 是否持有读锁。
    bool owns_lock() const noexcept
    {
        return m_owns;
    }
    /// @brief 同 owns_lock()。
    explicit operator bool() const noexcept
    {
        return m_owns;
    }
    /// @brief 取出关联的读写锁（可能为空）。
    SPtr mutex() const noexcept
    {
        return m_mutex;
    }

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

    /// @brief 构造一个不关联锁的空守卫。
    CoWriteLock() noexcept : m_mutex(nullptr), m_owns(false)
    {
    }

    /// @brief 构造即 WLock()；失败抛 std::system_error，不置 owns。
    explicit CoWriteLock(const SPtr& m) noexcept(false) : m_mutex(m), m_owns(false)
    {
        /* 同 CoReadLock：WLock 返回 -1 时抛 system_error，不置 owns。 */
        if (m_mutex->WLock() != 0)
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                    "CoWriteLock: WLock failed");
        m_owns = true;
    }

    /// @brief 只关联锁，不立即加锁。
    CoWriteLock(const SPtr& m, std::defer_lock_t) noexcept : m_mutex(m), m_owns(false)
    {
    }

    /// @brief 构造时 TryWLock() 一次，失败则保持未持锁。
    CoWriteLock(const SPtr& m, std::try_to_lock_t) noexcept : m_mutex(m), m_owns(false)
    {
        m_owns = (m_mutex->TryWLock() == 0);
    }

    /// @brief 接管调用方已持有的写锁，不再加锁。
    CoWriteLock(const SPtr& m, std::adopt_lock_t) noexcept : m_mutex(m), m_owns(true)
    {
    }

    /// @brief 移动构造：写锁所有权转给新对象，源对象变为未持锁。
    CoWriteLock(CoWriteLock&& other) noexcept : m_mutex(std::move(other.m_mutex)), m_owns(other.m_owns)
    {
        other.m_owns = false;
    }

    /// @brief 移动赋值：先释放自己持有的写锁，再接管源对象的写锁。
    CoWriteLock& operator=(CoWriteLock&& other) noexcept
    {
        if (m_owns)
            m_mutex->WUnLock();
        m_mutex = std::move(other.m_mutex);
        m_owns = other.m_owns;
        other.m_owns = false;
        return *this;
    }

    /// @brief 析构：持写锁时调用 WUnLock()，noexcept。
    ~CoWriteLock() noexcept
    {
        if (m_owns && m_mutex)
            m_mutex->WUnLock();
    }

    CoWriteLock(const CoWriteLock&) = delete;
    CoWriteLock& operator=(const CoWriteLock&) = delete;

    /// @brief 获取写锁；失败抛 std::system_error。
    void lock() noexcept(false)
    {
        AssertWithInfo(m_mutex != nullptr, "CoWriteLock::lock(): no associated mutex");
        AssertWithInfo(!m_owns, "CoWriteLock::lock(): already owns the write lock");
        if (m_mutex->WLock() != 0)
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                    "CoWriteLock::lock(): WLock failed");
        m_owns = true;
    }

    /// @brief TryWLock() 一次；返回是否拿到写锁。
    bool try_lock() noexcept
    {
        AssertWithInfo(m_mutex != nullptr, "CoWriteLock::try_lock(): no associated mutex");
        AssertWithInfo(!m_owns, "CoWriteLock::try_lock(): already owns the write lock");
        m_owns = (m_mutex->TryWLock() == 0);
        return m_owns;
    }

    /// @brief 在 ms 毫秒内 TryWLock(ms)；异常、超时、被抢先都折算成 false。
    bool try_lock_for(int ms) noexcept
    {
        AssertWithInfo(m_mutex != nullptr, "CoWriteLock::try_lock_for(): no associated mutex");
        AssertWithInfo(!m_owns, "CoWriteLock::try_lock_for(): already owns the write lock");
        /* 同读锁——等待准备可能抛异常，noexcept 内收敛为 false。 */
        try {
            m_owns = (m_mutex->TryWLock(ms) == 0);
        } catch (...) {
            m_owns = false;
        }
        return m_owns;
    }

    /// @brief 释放写锁；未持锁时断言。
    void unlock() noexcept
    {
        AssertWithInfo(m_mutex != nullptr, "CoWriteLock::unlock(): no associated mutex");
        AssertWithInfo(m_owns, "CoWriteLock::unlock(): does not own the write lock");
        m_mutex->WUnLock();
        m_owns = false;
    }

    /// @brief 是否持有写锁。
    bool owns_lock() const noexcept
    {
        return m_owns;
    }
    /// @brief 同 owns_lock()。
    explicit operator bool() const noexcept
    {
        return m_owns;
    }
    /// @brief 取出关联的读写锁（可能为空）。
    SPtr mutex() const noexcept
    {
        return m_mutex;
    }

private:
    SPtr m_mutex;
    bool m_owns;
};

} // namespace bbt::coroutine::sync
