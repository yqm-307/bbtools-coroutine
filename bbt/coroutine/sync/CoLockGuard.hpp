#pragma once
#include <bbt/core/util/Assert.hpp>
#include <memory>
#include <utility>

namespace bbt::coroutine::sync
{

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

} // namespace bbt::coroutine::sync
