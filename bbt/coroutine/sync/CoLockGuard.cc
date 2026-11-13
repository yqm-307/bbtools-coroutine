/**
 * @file CoLockGuard.cc
 * @brief CoReadLock / CoWriteLock 的成员实现，头文件只保留声明。
 */
#include <bbt/coroutine/sync/CoLockGuard.hpp>

namespace bbt::coroutine::sync {

CoReadLock::CoReadLock() noexcept : m_mutex(nullptr), m_owns(false) {}

CoReadLock::CoReadLock(const SPtr& m) noexcept(false) : m_mutex(m), m_owns(false)
{

    /**
         * RLock 返回 -1 时不得静默置 owns（无锁进临界区 / 析构误归还别人的锁），按 unique_lock 惯例抛 system_error。
         */
    if (m_mutex->RLock() != 0)
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                "CoReadLock: RLock failed");
    m_owns = true;
}

CoReadLock::CoReadLock(const SPtr& m, std::defer_lock_t) noexcept : m_mutex(m), m_owns(false) {}

CoReadLock::CoReadLock(const SPtr& m, std::try_to_lock_t) noexcept : m_mutex(m), m_owns(false)
{
    m_owns = (m_mutex->TryRLock() == 0);
}

CoReadLock::CoReadLock(const SPtr& m, std::adopt_lock_t) noexcept : m_mutex(m), m_owns(true) {}

CoReadLock::CoReadLock(CoReadLock&& other) noexcept : m_mutex(std::move(other.m_mutex)), m_owns(other.m_owns)
{
    other.m_owns = false;
}

CoReadLock& CoReadLock::operator=(CoReadLock&& other) noexcept
{
    if (m_owns)
        m_mutex->RUnLock();
    m_mutex = std::move(other.m_mutex);
    m_owns = other.m_owns;
    other.m_owns = false;
    return *this;
}

CoReadLock::~CoReadLock() noexcept
{
    if (m_owns && m_mutex)
        m_mutex->RUnLock();
}

void CoReadLock::lock() noexcept(false)
{
    AssertWithInfo(m_mutex != nullptr, "CoReadLock::lock(): no associated mutex");
    AssertWithInfo(!m_owns, "CoReadLock::lock(): already owns the read lock");

    /**
         * 同构造路径——登记失败以 system_error 抛出，不置 owns。
         */
    if (m_mutex->RLock() != 0)
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                "CoReadLock::lock(): RLock failed");
    m_owns = true;
}

bool CoReadLock::try_lock() noexcept
{
    AssertWithInfo(m_mutex != nullptr, "CoReadLock::try_lock(): no associated mutex");
    AssertWithInfo(!m_owns, "CoReadLock::try_lock(): already owns the read lock");
    m_owns = (m_mutex->TryRLock() == 0);
    return m_owns;
}

bool CoReadLock::try_lock_for(int ms) noexcept
{
    AssertWithInfo(m_mutex != nullptr, "CoReadLock::try_lock_for(): no associated mutex");
    AssertWithInfo(!m_owns, "CoReadLock::try_lock_for(): already owns the read lock");

    /**
         * 等待准备可能抛出（如分配失败）；noexcept 内收敛为 false。错误与超时在 bool 通道上不可分辨。
         */
    try {
        m_owns = (m_mutex->TryRLock(ms) == 0);
    } catch (...) {
        m_owns = false;
    }
    return m_owns;
}

void CoReadLock::unlock() noexcept
{
    AssertWithInfo(m_mutex != nullptr, "CoReadLock::unlock(): no associated mutex");
    AssertWithInfo(m_owns, "CoReadLock::unlock(): does not own the read lock");
    m_mutex->RUnLock();
    m_owns = false;
}

bool CoReadLock::owns_lock() const noexcept
{
    return m_owns;
}

CoReadLock::operator bool() const noexcept
{
    return m_owns;
}

CoReadLock::SPtr CoReadLock::mutex() const noexcept
{
    return m_mutex;
}

CoWriteLock::CoWriteLock() noexcept : m_mutex(nullptr), m_owns(false) {}

CoWriteLock::CoWriteLock(const SPtr& m) noexcept(false) : m_mutex(m), m_owns(false)
{

    /**
         * 同 CoReadLock：WLock 返回 -1 时抛 system_error，不置 owns。
         */
    if (m_mutex->WLock() != 0)
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                "CoWriteLock: WLock failed");
    m_owns = true;
}

CoWriteLock::CoWriteLock(const SPtr& m, std::defer_lock_t) noexcept : m_mutex(m), m_owns(false) {}

CoWriteLock::CoWriteLock(const SPtr& m, std::try_to_lock_t) noexcept : m_mutex(m), m_owns(false)
{
    m_owns = (m_mutex->TryWLock() == 0);
}

CoWriteLock::CoWriteLock(const SPtr& m, std::adopt_lock_t) noexcept : m_mutex(m), m_owns(true) {}

CoWriteLock::CoWriteLock(CoWriteLock&& other) noexcept : m_mutex(std::move(other.m_mutex)), m_owns(other.m_owns)
{
    other.m_owns = false;
}

CoWriteLock& CoWriteLock::operator=(CoWriteLock&& other) noexcept
{
    if (m_owns)
        m_mutex->WUnLock();
    m_mutex = std::move(other.m_mutex);
    m_owns = other.m_owns;
    other.m_owns = false;
    return *this;
}

CoWriteLock::~CoWriteLock() noexcept
{
    if (m_owns && m_mutex)
        m_mutex->WUnLock();
}

void CoWriteLock::lock() noexcept(false)
{
    AssertWithInfo(m_mutex != nullptr, "CoWriteLock::lock(): no associated mutex");
    AssertWithInfo(!m_owns, "CoWriteLock::lock(): already owns the write lock");
    if (m_mutex->WLock() != 0)
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
                                "CoWriteLock::lock(): WLock failed");
    m_owns = true;
}

bool CoWriteLock::try_lock() noexcept
{
    AssertWithInfo(m_mutex != nullptr, "CoWriteLock::try_lock(): no associated mutex");
    AssertWithInfo(!m_owns, "CoWriteLock::try_lock(): already owns the write lock");
    m_owns = (m_mutex->TryWLock() == 0);
    return m_owns;
}

bool CoWriteLock::try_lock_for(int ms) noexcept
{
    AssertWithInfo(m_mutex != nullptr, "CoWriteLock::try_lock_for(): no associated mutex");
    AssertWithInfo(!m_owns, "CoWriteLock::try_lock_for(): already owns the write lock");

    /**
         * 同读锁——等待准备可能抛异常，noexcept 内收敛为 false。
         */
    try {
        m_owns = (m_mutex->TryWLock(ms) == 0);
    } catch (...) {
        m_owns = false;
    }
    return m_owns;
}

void CoWriteLock::unlock() noexcept
{
    AssertWithInfo(m_mutex != nullptr, "CoWriteLock::unlock(): no associated mutex");
    AssertWithInfo(m_owns, "CoWriteLock::unlock(): does not own the write lock");
    m_mutex->WUnLock();
    m_owns = false;
}

bool CoWriteLock::owns_lock() const noexcept
{
    return m_owns;
}

CoWriteLock::operator bool() const noexcept
{
    return m_owns;
}

CoWriteLock::SPtr CoWriteLock::mutex() const noexcept
{
    return m_mutex;
}

}
