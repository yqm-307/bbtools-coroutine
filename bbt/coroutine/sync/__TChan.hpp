#pragma once
#include <bbt/base/assert/Assert.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>


namespace bbt::coroutine::sync
{

template<class TItem, int Max>
Chan<TItem, Max>::Chan():
    m_max_size(Max),
    m_enable_read_cond(CoCond::Create())
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

    std::unique_lock<std::mutex> lock{m_item_queue_mutex};

    /* 如果无法写入，协程挂起直到可写 */
    if (m_item_queue.size() >= m_max_size) {
        auto enable_write_cond = _CreateAndPushEnableWriteCond();

        if (_WaitUntilEnableWrite(enable_write_cond, [&](){ lock.unlock(); return true; }) != 0)
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
    // 如果信道关闭，不可以读了
    if (IsClosed())
        return -1;

    bool expect = false;
    if (!m_is_reading.compare_exchange_strong(expect, true))
        return -1;

    std::unique_lock<std::mutex> lock{m_item_queue_mutex};
    if (m_item_queue.empty()) {
        lock.unlock();

        if (_WaitUntilEnableRead() != 0)
            return -1;

        lock.lock();
    }
    
    item = m_item_queue.front();
    m_item_queue.pop();
    /* 抛出队列可写 */
    Assert(_OnEnableWrite() == 0);

    m_is_reading = false;

    return 0;
}


template<class TItem, int Max>
int Chan<TItem, Max>::ReadAll(std::vector<ItemType>& items)
{
    if (IsClosed())
        return -1;
    
    bool expect = false;
    if (!m_is_reading.compare_exchange_strong(expect, true))
        return -1;

    std::unique_lock<std::mutex> lock{m_item_queue_mutex};
    if (m_item_queue.empty()) {
        lock.unlock();

        _WaitUntilEnableRead();

        lock.lock();
    }

    while (!m_item_queue.empty()) {
        items.push_back(m_item_queue.front());
        m_item_queue.pop();
        Assert(_OnEnableWrite() == 0);
    }
    m_is_reading = false;

    return 0;
}

template<class TItem, int Max>
int Chan<TItem, Max>::TryWrite(const ItemType& item)
{
    if (IsClosed())
        return -1;

    std::unique_lock<std::mutex> _{m_item_queue_mutex};
    if (m_item_queue.size() >= m_max_size)
        return -1;
    
    m_item_queue.push(item);
    if (m_is_reading) _OnEnableRead();

    return 0;
}

template<class TItem, int Max>
int Chan<TItem, Max>::TryWrite(const ItemType& item, int timeout)
{
    if (IsClosed())
        return -1;

    std::unique_lock<std::mutex> lock{m_item_queue_mutex};
    if (m_item_queue.size() >= m_max_size) {
        auto enable_write_cond = _CreateAndPushEnableWriteCond();
        lock.unlock();

        int ret = _WaitUntilEnableWriteOrTimeout(enable_write_cond, timeout);

        
        lock.lock();
        if (ret == 1) {

        } else if (ret != 0) {
            return -1;
        }
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
        return -1;

    std::unique_lock<std::mutex> _{m_item_queue_mutex};

    if (m_item_queue.empty())
        return -1;
    
    item = m_item_queue.front();
    m_item_queue.pop();
    Assert(_OnEnableWrite() == 0);

    return 0;
}

template<class TItem, int Max>
int Chan<TItem, Max>::TryRead(ItemType& item, int timeout)
{
    if (IsClosed())
        return -1;
    
    bool expect = false;
    if (!m_is_reading.compare_exchange_strong(expect, true))
        return -1;

    std::unique_lock<std::mutex> lock{m_item_queue_mutex};
    if (m_item_queue.empty()) {
        lock.unlock();

        if (_WaitUntilEnableReadOrTimeout(timeout) != 0)
            return -1;

        lock.lock();
    }

    if (m_item_queue.empty()) {
        m_item_queue_mutex.unlock();
        return -1;
    }

    item = m_item_queue.front();
    m_item_queue.pop();
    Assert(_OnEnableWrite() == 0);

    m_is_reading = false;

    return 0;
}

template<class TItem, int Max>
void Chan<TItem, Max>::Close()
{
    if (IsClosed())
        return;

    m_run_status = ChanStatus::CHAN_CLOSE;

    std::unique_lock<std::mutex> _{m_item_queue_mutex};
    /* 唤醒所有阻塞在Channel上的协程 */
    if (m_is_reading) _OnEnableRead();

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

template<class TItem, int Max>
int Chan<TItem, Max>::_WaitUntilEnableRead()
{
    return m_enable_read_cond->Wait();
}

template<class TItem, int Max>
int Chan<TItem, Max>::_WaitUntilEnableWrite(CoCond::SPtr cond, const detail::CoroutineOnYieldCallback& cb)
{
    return cond->WaitWithCallback(std::forward<const detail::CoroutineOnYieldCallback&>(cb));
}

template<class TItem, int Max>
int Chan<TItem, Max>::_WaitUntilEnableWriteOrTimeout(CoCond::SPtr cond, int timeout_ms)
{
    return cond->WaitWithTimeout(timeout_ms);
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
CoCond::SPtr Chan<TItem, Max>::_CreateAndPushEnableWriteCond()
{
    auto enable_write_cond = CoCond::Create();
    Assert(enable_write_cond != nullptr);
    m_enable_write_conds.push(enable_write_cond);

    return enable_write_cond;
}

template<class TItem, int Max>
int Chan<TItem, Max>::_WaitUntilEnableReadOrTimeout(int timeout_ms)
{
    return m_enable_read_cond->WaitWithTimeout(timeout_ms);
}

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
// Chan no cache

template<class TItem>
int Chan<TItem, 0>::Write(const ItemType& item)
{
    if (BaseType::IsClosed())
        return -1;

    std::unique_lock<std::mutex> lock{BaseType::m_item_queue_mutex};

    /* 加入引用队列，并尝试唤醒挂起的读端，如果没有正在读的协程挂起等待 */
    m_item_cache_ref.push(item);
    m_write_idx++;
    auto is_writing_idx = m_write_idx - 1;

    if (BaseType::m_is_reading && (is_writing_idx == m_read_idx)) {
        BaseType::_OnEnableRead();
        return 0;
    }

    auto enable_write_cond = BaseType::_CreateAndPushEnableWriteCond();

    if (BaseType::_WaitUntilEnableWrite(enable_write_cond, [&](){ lock.unlock(); return true; }) != 0)
        return -1;
    
    return 0;
}

template<class TItem>
int Chan<TItem, 0>::Read(ItemType& item)
{
    // 如果信道关闭，不可以读了
    if (BaseType::IsClosed())
        return -1;

    bool expect = false;
    if (!BaseType::m_is_reading.compare_exchange_strong(expect, true))
        return -1;

    std::unique_lock<std::mutex> lock{BaseType::m_item_queue_mutex};

    if (m_write_idx <= m_read_idx) {
        lock.unlock();

        if (BaseType::_WaitUntilEnableRead() != 0)
            return -1;

        lock.lock();
    }
    
    item = m_item_cache_ref.front();
    m_item_cache_ref.pop();
    m_read_idx++;
    /* 抛出队列可写 */
    Assert(BaseType::_OnEnableWrite() == 0);

    BaseType::m_is_reading = false;

    return 0;
}

}