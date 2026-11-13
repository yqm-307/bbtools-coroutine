/**
 * @file __TCoLockGuard.hpp
 * @brief CoLockGuard / CoUniqueLock 的模板实现，由 CoLockGuard.hpp 末尾包含。
 */
#pragma once
#include <bbt/coroutine/sync/CoLockGuard.hpp>

namespace bbt::coroutine::sync {

template <typename Mutex> CoLockGuard<Mutex>::CoLockGuard(const std::shared_ptr<Mutex>& m) noexcept(false) : m_mutex(m)
{
    m_mutex->Lock();
}

template <typename Mutex> CoLockGuard<Mutex>::~CoLockGuard() noexcept
{
    if (m_mutex)
        m_mutex->UnLock();
}

template <typename Mutex> CoUniqueLock<Mutex>::CoUniqueLock() noexcept : m_mutex(nullptr), m_owns(false) {}

template <typename Mutex>
CoUniqueLock<Mutex>::CoUniqueLock(const std::shared_ptr<Mutex>& m) noexcept(false) : m_mutex(m), m_owns(false)
{
    m_mutex->Lock();
    m_owns = true;
}

template <typename Mutex>
CoUniqueLock<Mutex>::CoUniqueLock(const std::shared_ptr<Mutex>& m, std::defer_lock_t) noexcept
    : m_mutex(m), m_owns(false)
{
}

template <typename Mutex>
CoUniqueLock<Mutex>::CoUniqueLock(const std::shared_ptr<Mutex>& m, std::try_to_lock_t) noexcept
    : m_mutex(m), m_owns(false)
{
    m_owns = (m_mutex->TryLock() == 0);
}

template <typename Mutex>
CoUniqueLock<Mutex>::CoUniqueLock(const std::shared_ptr<Mutex>& m, std::adopt_lock_t) noexcept
    : m_mutex(m), m_owns(true)
{
}

template <typename Mutex>
CoUniqueLock<Mutex>::CoUniqueLock(CoUniqueLock&& other) noexcept
    : m_mutex(std::move(other.m_mutex)), m_owns(other.m_owns)
{
    other.m_owns = false;
}

template <typename Mutex> CoUniqueLock<Mutex>& CoUniqueLock<Mutex>::operator=(CoUniqueLock&& other) noexcept
{
    if (m_owns)
        m_mutex->UnLock();
    m_mutex = std::move(other.m_mutex);
    m_owns = other.m_owns;
    other.m_owns = false;
    return *this;
}

template <typename Mutex> CoUniqueLock<Mutex>::~CoUniqueLock() noexcept
{
    if (m_owns && m_mutex)
        m_mutex->UnLock();
}

template <typename Mutex> void CoUniqueLock<Mutex>::lock() noexcept(false)
{
    AssertWithInfo(m_mutex != nullptr, "CoUniqueLock::lock(): no associated mutex");
    AssertWithInfo(!m_owns, "CoUniqueLock::lock(): already owns the lock (deadlock would occur)");
    m_mutex->Lock();
    m_owns = true;
}

template <typename Mutex> bool CoUniqueLock<Mutex>::try_lock() noexcept
{
    AssertWithInfo(m_mutex != nullptr, "CoUniqueLock::try_lock(): no associated mutex");
    AssertWithInfo(!m_owns, "CoUniqueLock::try_lock(): already owns the lock");
    m_owns = (m_mutex->TryLock() == 0);
    return m_owns;
}

template <typename Mutex> bool CoUniqueLock<Mutex>::try_lock_for(int ms) noexcept
{
    AssertWithInfo(m_mutex != nullptr, "CoUniqueLock::try_lock_for(): no associated mutex");
    AssertWithInfo(!m_owns, "CoUniqueLock::try_lock_for(): already owns the lock");

    /**
         * 等待准备可能抛出（如分配失败）；noexcept 内收敛为未获锁，不 terminate。
         */
    try {
        m_owns = (m_mutex->TryLock(ms) == 0);
    } catch (...) {
        m_owns = false;
    }
    return m_owns;
}

template <typename Mutex> void CoUniqueLock<Mutex>::unlock() noexcept
{
    AssertWithInfo(m_mutex != nullptr, "CoUniqueLock::unlock(): no associated mutex");
    AssertWithInfo(m_owns, "CoUniqueLock::unlock(): does not own the lock");
    m_mutex->UnLock();
    m_owns = false;
}

template <typename Mutex> bool CoUniqueLock<Mutex>::owns_lock() const noexcept
{
    return m_owns;
}

template <typename Mutex> CoUniqueLock<Mutex>::operator bool() const noexcept
{
    return m_owns;
}

template <typename Mutex> std::shared_ptr<Mutex> CoUniqueLock<Mutex>::mutex() const noexcept
{
    return m_mutex;
}

}
