#pragma once
#include <bbt/core/clock/Clock.hpp>
#include <bbt/core/util/Assert.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>


namespace bbt::coroutine::sync
{

// ============================================================
// inline helpers
// ============================================================

// RAII guard: 异常路径自动重置 m_is_reading
struct ChanReadGuard {
    std::atomic_bool& flag;
    ~ChanReadGuard() { flag.store(false, std::memory_order_release); }
};

// ============================================================
// buffered Chan (Max > 0)
// ============================================================

template<class TItem, int Max>
Chan<TItem, Max>::Chan():
    m_max_size(Max),
    m_enable_read_cond(CoWaiter::Create())
{
    Assert(m_max_size >= 0);
    m_run_status = ChanStatus::CHAN_OPEN;
}

template<class TItem, int Max>
Chan<TItem, Max>::~Chan()
{
    if (!IsClosed())
        Close();
}

template<class TItem, int Max>
int Chan<TItem, Max>::Write(const ItemType& item)
{
    if (IsClosed())
        return -1;

    std::unique_lock<std::mutex> lock(m_item_queue_mutex);

    while (m_item_queue.size() >= (size_t)m_max_size) {
        auto enable_write_cond = _CreateAndPushEnableWriteCond();

        lock.unlock();
        if (_WaitUntilEnableWrite(enable_write_cond, [](){ return true; }) != 0)
            return -2;

        if (IsClosed())
            return -1;

        lock.lock();
    }

    m_item_queue.push(item);
    if (m_is_reading)
        _OnEnableRead();

    return 0;
}

template<class TItem, int Max>
int Chan<TItem, Max>::Read(ItemType& item)
{
    bool expect = false;
    if (!m_is_reading.compare_exchange_strong(expect, true))
        return -2;

    ChanReadGuard guard{m_is_reading};

    if (IsClosed()) {
        std::lock_guard<std::mutex> lock(m_item_queue_mutex);
        if (m_item_queue.empty())
            return -1;
    }

    std::unique_lock<std::mutex> lock(m_item_queue_mutex);

    while (m_item_queue.empty() && !IsClosed()) {
        lock.unlock();
        int ret = _WaitUntilEnableReadOrTimeout(500, [](){ return true; });
        lock.lock();
        if (ret != 0 && ret != 1)
            return -2;
    }

    if (m_item_queue.empty())
        return -1;

    item = m_item_queue.front();
    m_item_queue.pop();

    // 唤醒一个等待写入的协程（忽略 -1，可能已被 Cancel）
    _OnEnableWrite();

    return 0;
}

template<class TItem, int Max>
int Chan<TItem, Max>::ReadAll(std::vector<ItemType>& items)
{
    bool expect = false;
    if (!m_is_reading.compare_exchange_strong(expect, true))
        return -2;

    ChanReadGuard guard{m_is_reading};

    std::unique_lock<std::mutex> lock(m_item_queue_mutex);

    while (m_item_queue.empty() && !IsClosed()) {
        lock.unlock();
        int ret = _WaitUntilEnableReadOrTimeout(500, [](){ return true; });
        lock.lock();
        if (ret != 0 && ret != 1)
            return -2;
    }

    if (m_item_queue.empty())
        return -1;  // 关闭+空

    while (!m_item_queue.empty()) {
        items.push_back(m_item_queue.front());
        m_item_queue.pop();
        _OnEnableWrite();
    }

    return 0;
}

template<class TItem, int Max>
int Chan<TItem, Max>::TryWrite(const ItemType& item)
{
    if (IsClosed())
        return -1;

    std::lock_guard<std::mutex> lock(m_item_queue_mutex);
    if (m_item_queue.size() >= (size_t)m_max_size)
        return -2;

    m_item_queue.push(item);
    if (m_is_reading)
        _OnEnableRead();

    return 0;
}

template<class TItem, int Max>
int Chan<TItem, Max>::TryWrite(const ItemType& item, int timeout)
{
    if (IsClosed())
        return -1;

    auto start = bbt::core::clock::gettime_mono();
    std::unique_lock<std::mutex> lock(m_item_queue_mutex);

    while (m_item_queue.size() >= (size_t)m_max_size) {
        int elapsed = bbt::core::clock::gettime_mono() - start;
        int remaining = timeout - elapsed;
        if (remaining <= 0)
            return 1;  // 超时

        auto enable_write_cond = _CreateAndPushEnableWriteCond();
        lock.unlock();
        int ret = _WaitUntilEnableWriteOrTimeout(
            enable_write_cond, remaining,
            [](){ return true; });

        if (ret != 0)
            return (ret == 1) ? 1 : -2;

        if (IsClosed())
            return -1;

        lock.lock();
    }

    m_item_queue.push(item);
    if (m_is_reading)
        _OnEnableRead();

    return 0;
}

template<class TItem, int Max>
int Chan<TItem, Max>::TryRead(ItemType& item)
{
    if (IsClosed())
        return -1;

    bool expect = false;
    if (!m_is_reading.compare_exchange_strong(expect, true))
        return -2;

    ChanReadGuard guard{m_is_reading};

    std::lock_guard<std::mutex> lock(m_item_queue_mutex);

    if (m_item_queue.empty())
        return -2;  // 空

    item = m_item_queue.front();
    m_item_queue.pop();
    _OnEnableWrite();

    return 0;
}

template<class TItem, int Max>
int Chan<TItem, Max>::TryRead(ItemType& item, int timeout)
{
    if (IsClosed())
        return -1;

    bool expect = false;
    if (!m_is_reading.compare_exchange_strong(expect, true))
        return -2;

    ChanReadGuard guard{m_is_reading};

    auto start = bbt::core::clock::gettime_mono();
    std::unique_lock<std::mutex> lock(m_item_queue_mutex);

    while (m_item_queue.empty() && !IsClosed()) {
        int elapsed = bbt::core::clock::gettime_mono() - start;
        int remaining = timeout - elapsed;
        if (remaining <= 0)
            return 1;  // 超时

        lock.unlock();
        int ret = _WaitUntilEnableReadOrTimeout(remaining,
                [](){ return true; });
        lock.lock();
        if (ret != 0 && ret != 1)
            return -2;
    }

    if (m_item_queue.empty())
        return -1;  // 关闭+空

    item = m_item_queue.front();
    m_item_queue.pop();
    _OnEnableWrite();

    return 0;
}

template<class TItem, int Max>
void Chan<TItem, Max>::Close()
{
    if (IsClosed())
        return;

    m_run_status = ChanStatus::CHAN_CLOSE;

    std::lock_guard<std::mutex> lock(m_item_queue_mutex);

    // 不再丢弃缓冲数据 — 保留给读者读完
    // 唤醒所有阻塞的协程
    if (m_is_reading)
        _OnEnableRead();

    while (!m_enable_write_conds.empty()) {
        auto enable_write_cond = m_enable_write_conds.front();
        m_enable_write_conds.pop();
        enable_write_cond->Notify();
    }
}

template<class TItem, int Max>
bool Chan<TItem, Max>::IsClosed()
{
    return (m_run_status == ChanStatus::CHAN_CLOSE);
}

// ============================================================
// wait helpers (unchanged logic, adapted for unique_lock)
// ============================================================

template<class TItem, int Max>
int Chan<TItem, Max>::_WaitUntilEnableRead(const detail::CoroutineOnYieldCallback& cb)
{
    return m_enable_read_cond->WaitWithCallback(cb);
}

template<class TItem, int Max>
int Chan<TItem, Max>::_WaitUntilEnableWrite(CoWaiter::SPtr cond, const detail::CoroutineOnYieldCallback& cb)
{
    return cond->WaitWithCallback(cb);
}

template<class TItem, int Max>
int Chan<TItem, Max>::_WaitUntilEnableWriteOrTimeout(
    CoWaiter::SPtr cond, int timeout_ms, const detail::CoroutineOnYieldCallback& cb)
{
    return cond->WaitWithTimeoutAndCallback(timeout_ms, cb);
}

template<class TItem, int Max>
int Chan<TItem, Max>::_OnEnableRead()
{
    return m_enable_read_cond->Notify();
}

template<class TItem, int Max>
int Chan<TItem, Max>::_OnEnableWrite()
{
    if (m_enable_write_conds.empty())
        return 0;

    auto enable_write_cond = m_enable_write_conds.front();
    m_enable_write_conds.pop();
    return enable_write_cond->Notify();
}

template<class TItem, int Max>
CoWaiter::SPtr Chan<TItem, Max>::_CreateAndPushEnableWriteCond()
{
    auto enable_write_cond = CoWaiter::Create();
    Assert(enable_write_cond != nullptr);
    m_enable_write_conds.push(enable_write_cond);
    return enable_write_cond;
}

template<class TItem, int Max>
int Chan<TItem, Max>::_WaitUntilEnableReadOrTimeout(
    int timeout_ms, const detail::CoroutineOnYieldCallback& cb)
{
    return m_enable_read_cond->WaitWithTimeoutAndCallback(timeout_ms, cb);
}

// ============================================================
// stream operators
// ============================================================

template<class TItem, int Max>
bool operator<<(Chan<TItem, Max>& chan, const typename Chan<TItem, Max>::ItemType& item)
{
    return (chan.Write(item) == 0);
}

template<class TItem, int Max>
bool operator>>(Chan<TItem, Max>& chan, typename Chan<TItem, Max>::ItemType& item)
{
    return (chan.Read(item) == 0);
}

template<class TItem, int Max>
bool operator<<(std::shared_ptr<Chan<TItem, Max>> chan, const TItem& item)
{
    return (chan->Write(item) == 0);
}

template<class TItem, int Max>
bool operator>>(std::shared_ptr<Chan<TItem, Max>> chan, TItem& item)
{
    return (chan->Read(item) == 0);
}

///////////////////////////////////////////////////
// Chan no cache (Chan<T, 0>)
///////////////////////////////////////////////////

template<class TItem>
int Chan<TItem, 0>::Write(const ItemType& item)
{
    if (IsClosed())
        return -1;

    std::unique_lock<std::mutex> lock(BaseType::m_item_queue_mutex);

    /* 加入引用队列，并尝试唤醒挂起的读端 */
    m_item_cache_ref.push(item);
    m_write_idx++;
    auto is_writing_idx = m_write_idx - 1;

    if (BaseType::m_is_reading && (is_writing_idx == m_read_idx)) {
        BaseType::_OnEnableRead();
        return 0;
    }

    auto enable_write_cond = BaseType::_CreateAndPushEnableWriteCond();

    lock.unlock();
    if (BaseType::_WaitUntilEnableWrite(enable_write_cond,
            [](){ return true; }) != 0)
        return -2;

    return 0;
}

template<class TItem>
int Chan<TItem, 0>::Read(ItemType& item)
{
    bool expect = false;
    if (!BaseType::m_is_reading.compare_exchange_strong(expect, true))
        return -2;

    ChanReadGuard guard{BaseType::m_is_reading};

    if (IsClosed()) {
        std::lock_guard<std::mutex> lock(BaseType::m_item_queue_mutex);
        if (m_write_idx <= m_read_idx)
            return -1;
    }

    std::unique_lock<std::mutex> lock(BaseType::m_item_queue_mutex);

    while (m_write_idx <= m_read_idx && !IsClosed()) {
        lock.unlock();
        if (BaseType::_WaitUntilEnableRead(
                [](){ return true; }) != 0)
            return -2;
        lock.lock();
    }

    if (m_write_idx <= m_read_idx)
        return -1;  // 关闭+空

    item = m_item_cache_ref.front();
    m_item_cache_ref.pop();
    m_read_idx++;

    BaseType::_OnEnableWrite();

    return 0;
}

template<class TItem>
void Chan<TItem, 0>::Close()
{
    if (IsClosed())
        return;

    BaseType::m_run_status = ChanStatus::CHAN_CLOSE;

    std::lock_guard<std::mutex> lock(BaseType::m_item_queue_mutex);

    if (BaseType::m_is_reading)
        BaseType::_OnEnableRead();

    while (!BaseType::m_enable_write_conds.empty()) {
        auto enable_write_cond = BaseType::m_enable_write_conds.front();
        BaseType::m_enable_write_conds.pop();
        enable_write_cond->Notify();
    }
}

template<class TItem>
bool Chan<TItem, 0>::IsClosed()
{
    return (BaseType::m_run_status == ChanStatus::CHAN_CLOSE);
}

} // namespace bbt::coroutine::sync
