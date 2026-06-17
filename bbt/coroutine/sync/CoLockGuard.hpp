#pragma once
#include <bbt/core/util/Assert.hpp>
#include <memory>
#include <utility>

namespace bbt::coroutine::sync
{

// 前向声明，供 CoReadLock / CoWriteLock 使用
class CoRWMutex;

/**
 * @brief RAII 协程锁守卫，构造时 Lock，析构时 UnLock
 * 
 * 类似 std::lock_guard，持有 shared_ptr 保证锁生命周期。
 * 不可拷贝、不可移动。
 * 
 * 析构函数 noexcept — 异常路径下也不会抛异常。
 * 死锁检测由底层 Mutex::Lock() 内部的 assert 实现。
 * 
 * @tparam Mutex 满足 Lockable 概念：Lock() / UnLock()
 */
template<typename Mutex>
class CoLockGuard
{
public:
    explicit CoLockGuard(const std::shared_ptr<Mutex>& m) noexcept(false)
        : m_mutex(m)
    {
        m_mutex->Lock();
    }

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
 * @brief RAII 协程锁，支持延迟锁定、try_lock、移动语义
 * 
 * 类似 std::unique_lock，持有 shared_ptr 保证锁生命周期。
 * 提供更灵活的锁管理：
 * - 延迟锁定（defer_lock）
 * - 尝试锁定（try_to_lock / try_lock_for）
 * - 移动语义（可在作用域间转移锁所有权）
 * - 手动 unlock / lock
 * 
 * 析构函数 noexcept — 仅在持有锁时调用 UnLock()。
 * 
 * @tparam Mutex 满足 Lockable 概念：Lock() / UnLock() / TryLock() / TryLock(int)
 */
template<typename Mutex>
class CoUniqueLock
{
public:
    CoUniqueLock() noexcept
        : m_mutex(nullptr)
        , m_owns(false)
    {}

    explicit CoUniqueLock(const std::shared_ptr<Mutex>& m) noexcept(false)
        : m_mutex(m)
        , m_owns(false)
    {
        m_mutex->Lock();
        m_owns = true;
    }

    CoUniqueLock(const std::shared_ptr<Mutex>& m, std::defer_lock_t) noexcept
        : m_mutex(m)
        , m_owns(false)
    {}

    CoUniqueLock(const std::shared_ptr<Mutex>& m, std::try_to_lock_t) noexcept
        : m_mutex(m)
        , m_owns(false)
    {
        m_owns = (m_mutex->TryLock() == 0);
    }

    CoUniqueLock(const std::shared_ptr<Mutex>& m, std::adopt_lock_t) noexcept
        : m_mutex(m)
        , m_owns(true)
    {}

    CoUniqueLock(CoUniqueLock&& other) noexcept
        : m_mutex(std::move(other.m_mutex))
        , m_owns(other.m_owns)
    {
        other.m_owns = false;
    }

    CoUniqueLock& operator=(CoUniqueLock&& other) noexcept
    {
        if (m_owns)
            m_mutex->UnLock();
        m_mutex = std::move(other.m_mutex);
        m_owns = other.m_owns;
        other.m_owns = false;
        return *this;
    }

    ~CoUniqueLock() noexcept
    {
        if (m_owns && m_mutex)
            m_mutex->UnLock();
    }

    CoUniqueLock(const CoUniqueLock&) = delete;
    CoUniqueLock& operator=(const CoUniqueLock&) = delete;

    void lock() noexcept(false)
    {
        AssertWithInfo(m_mutex != nullptr, "CoUniqueLock::lock(): no associated mutex");
        AssertWithInfo(!m_owns, "CoUniqueLock::lock(): already owns the lock (deadlock would occur)");
        m_mutex->Lock();
        m_owns = true;
    }

    bool try_lock() noexcept
    {
        AssertWithInfo(m_mutex != nullptr, "CoUniqueLock::try_lock(): no associated mutex");
        AssertWithInfo(!m_owns, "CoUniqueLock::try_lock(): already owns the lock");
        m_owns = (m_mutex->TryLock() == 0);
        return m_owns;
    }

    bool try_lock_for(int ms) noexcept
    {
        AssertWithInfo(m_mutex != nullptr, "CoUniqueLock::try_lock_for(): no associated mutex");
        AssertWithInfo(!m_owns, "CoUniqueLock::try_lock_for(): already owns the lock");
        m_owns = (m_mutex->TryLock(ms) == 0);
        return m_owns;
    }

    void unlock() noexcept
    {
        AssertWithInfo(m_mutex != nullptr, "CoUniqueLock::unlock(): no associated mutex");
        AssertWithInfo(m_owns, "CoUniqueLock::unlock(): does not own the lock");
        m_mutex->UnLock();
        m_owns = false;
    }

    bool owns_lock() const noexcept { return m_owns; }
    explicit operator bool() const noexcept { return m_owns; }
    std::shared_ptr<Mutex> mutex() const noexcept { return m_mutex; }

private:
    std::shared_ptr<Mutex> m_mutex;
    bool                   m_owns;
};

// ============================================================================
// CoRWMutex 专用 RAII 守卫
// ============================================================================

/**
 * @brief RAII 读锁守卫
 * 
 * 构造时调用 CoRWMutex::RLock()，析构时调用 RUnLock() (noexcept)。
 * 支持 defer_lock / try_to_lock / adopt_lock / try_lock_for(ms) / 移动语义。
 * 
 * 不可拷贝，支持移动。
 * 
 * @note ReadGuard 作用域内获取写锁会触发 CoRWMutex 内部死锁检测 assert。
 */
class CoReadLock
{
public:
    CoReadLock() noexcept
        : m_mutex(nullptr)
        , m_owns(false)
    {}

    explicit CoReadLock(CoRWMutex& m) noexcept(false)
        : m_mutex(&m)
        , m_owns(false)
    {
        m_mutex->RLock();
        m_owns = true;
    }

    CoReadLock(CoRWMutex& m, std::defer_lock_t) noexcept
        : m_mutex(&m)
        , m_owns(false)
    {}

    CoReadLock(CoRWMutex& m, std::try_to_lock_t) noexcept
        : m_mutex(&m)
        , m_owns(false)
    {
        m_owns = (m_mutex->TryRLock() == 0);
    }

    CoReadLock(CoRWMutex& m, std::adopt_lock_t) noexcept
        : m_mutex(&m)
        , m_owns(true)
    {}

    CoReadLock(CoReadLock&& other) noexcept
        : m_mutex(other.m_mutex)
        , m_owns(other.m_owns)
    {
        other.m_mutex = nullptr;
        other.m_owns = false;
    }

    CoReadLock& operator=(CoReadLock&& other) noexcept
    {
        if (m_owns)
            m_mutex->RUnLock();
        m_mutex = other.m_mutex;
        m_owns = other.m_owns;
        other.m_mutex = nullptr;
        other.m_owns = false;
        return *this;
    }

    ~CoReadLock() noexcept
    {
        if (m_owns)
            m_mutex->RUnLock();
    }

    CoReadLock(const CoReadLock&) = delete;
    CoReadLock& operator=(const CoReadLock&) = delete;

    void lock() noexcept(false)
    {
        AssertWithInfo(m_mutex != nullptr, "CoReadLock::lock(): no associated mutex");
        AssertWithInfo(!m_owns, "CoReadLock::lock(): already owns the read lock");
        m_mutex->RLock();
        m_owns = true;
    }

    bool try_lock() noexcept
    {
        AssertWithInfo(m_mutex != nullptr, "CoReadLock::try_lock(): no associated mutex");
        AssertWithInfo(!m_owns, "CoReadLock::try_lock(): already owns the read lock");
        m_owns = (m_mutex->TryRLock() == 0);
        return m_owns;
    }

    bool try_lock_for(int ms) noexcept
    {
        AssertWithInfo(m_mutex != nullptr, "CoReadLock::try_lock_for(): no associated mutex");
        AssertWithInfo(!m_owns, "CoReadLock::try_lock_for(): already owns the read lock");
        m_owns = (m_mutex->TryRLock(ms) == 0);
        return m_owns;
    }

    void unlock() noexcept
    {
        AssertWithInfo(m_mutex != nullptr, "CoReadLock::unlock(): no associated mutex");
        AssertWithInfo(m_owns, "CoReadLock::unlock(): does not own the read lock");
        m_mutex->RUnLock();
        m_owns = false;
    }

    bool owns_lock() const noexcept { return m_owns; }
    explicit operator bool() const noexcept { return m_owns; }
    CoRWMutex* mutex() const noexcept { return m_mutex; }

private:
    CoRWMutex* m_mutex;
    bool       m_owns;
};

/**
 * @brief RAII 写锁守卫
 * 
 * 构造时调用 CoRWMutex::WLock()，析构时调用 WUnLock() (noexcept)。
 * 支持 defer_lock / try_to_lock / adopt_lock / try_lock_for(ms) / 移动语义。
 * 
 * 不可拷贝，支持移动。
 * 
 * @note 持有读锁时再获取写锁会触发 CoRWMutex 内部死锁检测 assert（不允许读锁升级）。
 */
class CoWriteLock
{
public:
    CoWriteLock() noexcept
        : m_mutex(nullptr)
        , m_owns(false)
    {}

    explicit CoWriteLock(CoRWMutex& m) noexcept(false)
        : m_mutex(&m)
        , m_owns(false)
    {
        m_mutex->WLock();
        m_owns = true;
    }

    CoWriteLock(CoRWMutex& m, std::defer_lock_t) noexcept
        : m_mutex(&m)
        , m_owns(false)
    {}

    CoWriteLock(CoRWMutex& m, std::try_to_lock_t) noexcept
        : m_mutex(&m)
        , m_owns(false)
    {
        m_owns = (m_mutex->TryWLock() == 0);
    }

    CoWriteLock(CoRWMutex& m, std::adopt_lock_t) noexcept
        : m_mutex(&m)
        , m_owns(true)
    {}

    CoWriteLock(CoWriteLock&& other) noexcept
        : m_mutex(other.m_mutex)
        , m_owns(other.m_owns)
    {
        other.m_mutex = nullptr;
        other.m_owns = false;
    }

    CoWriteLock& operator=(CoWriteLock&& other) noexcept
    {
        if (m_owns)
            m_mutex->WUnLock();
        m_mutex = other.m_mutex;
        m_owns = other.m_owns;
        other.m_mutex = nullptr;
        other.m_owns = false;
        return *this;
    }

    ~CoWriteLock() noexcept
    {
        if (m_owns)
            m_mutex->WUnLock();
    }

    CoWriteLock(const CoWriteLock&) = delete;
    CoWriteLock& operator=(const CoWriteLock&) = delete;

    void lock() noexcept(false)
    {
        AssertWithInfo(m_mutex != nullptr, "CoWriteLock::lock(): no associated mutex");
        AssertWithInfo(!m_owns, "CoWriteLock::lock(): already owns the write lock");
        m_mutex->WLock();
        m_owns = true;
    }

    bool try_lock() noexcept
    {
        AssertWithInfo(m_mutex != nullptr, "CoWriteLock::try_lock(): no associated mutex");
        AssertWithInfo(!m_owns, "CoWriteLock::try_lock(): already owns the write lock");
        m_owns = (m_mutex->TryWLock() == 0);
        return m_owns;
    }

    bool try_lock_for(int ms) noexcept
    {
        AssertWithInfo(m_mutex != nullptr, "CoWriteLock::try_lock_for(): no associated mutex");
        AssertWithInfo(!m_owns, "CoWriteLock::try_lock_for(): already owns the write lock");
        m_owns = (m_mutex->TryWLock(ms) == 0);
        return m_owns;
    }

    void unlock() noexcept
    {
        AssertWithInfo(m_mutex != nullptr, "CoWriteLock::unlock(): no associated mutex");
        AssertWithInfo(m_owns, "CoWriteLock::unlock(): does not own the write lock");
        m_mutex->WUnLock();
        m_owns = false;
    }

    bool owns_lock() const noexcept { return m_owns; }
    explicit operator bool() const noexcept { return m_owns; }
    CoRWMutex* mutex() const noexcept { return m_mutex; }

private:
    CoRWMutex* m_mutex;
    bool       m_owns;
};

} // namespace bbt::coroutine::sync
