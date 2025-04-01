#pragma once
#include <bbt/core/util/Assert.hpp>
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

    _Lock();

    /* 如果无法写入，协程挂起直到可写 */
    if (m_item_queue.size() >= m_max_size) {
        auto enable_write_cond = _CreateAndPushEnableWriteCond();

        if (_WaitUntilEnableWrite(enable_write_cond, [this](){ _UnLock(); return true; }) != 0)
            return -1;

        _Lock();
    }
    
    m_item_queue.push(item);
    if (m_is_reading)
        _OnEnableRead();

    _UnLock();
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

    _Lock();

    if (m_item_queue.empty()) {
        if (_WaitUntilEnableRead([this](){ _UnLock(); return true; }) != 0)
            return -1;

        _Lock();
    }
    
    item = m_item_queue.front();
    m_item_queue.pop();
    /* 抛出队列可写 */
    Assert(_OnEnableWrite() == 0);
    m_is_reading.exchange(false);
    _UnLock();
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

    _Lock();
    if (m_item_queue.empty()) {

        if (_WaitUntilEnableRead([this](){ _UnLock(); return true; }) != 0)
            return -1;

        _Lock();
    }

    while (!m_item_queue.empty()) {
        items.push_back(m_item_queue.front());
        m_item_queue.pop();
        Assert(_OnEnableWrite() == 0);
    }
    m_is_reading = false;

    _UnLock();
    return 0;
}

template<class TItem, int Max>
int Chan<TItem, Max>::TryWrite(const ItemType& item)
{
    if (IsClosed())
        return -1;

    _Lock();
    if (m_item_queue.size() >= m_max_size) {
        _UnLock();
        return -1;
    }
    
    m_item_queue.push(item);
    if (m_is_reading) _OnEnableRead();

    _UnLock();
    return 0;
}

template<class TItem, int Max>
int Chan<TItem, Max>::TryWrite(const ItemType& item, int timeout)
{
    int ret = 0;

    if (IsClosed())
        return -1;

    _Lock();
    if (m_item_queue.size() >= m_max_size) {
        auto enable_write_cond = _CreateAndPushEnableWriteCond();
        ret = _WaitUntilEnableWriteOrTimeout(enable_write_cond, timeout, [this](){ _UnLock(); return true; });

        _Lock();
    }

    m_item_queue.push(item);
    if (m_is_reading)
        _OnEnableRead();

    _UnLock();
    return ret;
}

template<class TItem, int Max>
int Chan<TItem, Max>::TryRead(ItemType& item)
{
    if (IsClosed())
        return -1;
    
    bool expect = false;
    if (!m_is_reading.compare_exchange_strong(expect, true))
        return -1;

    _Lock();

    if (m_item_queue.empty())
        return -1;
    
    item = m_item_queue.front();
    m_item_queue.pop();
    Assert(_OnEnableWrite() == 0);

    _UnLock();

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

    _Lock();
    if (m_item_queue.empty()) {

        if (_WaitUntilEnableReadOrTimeout(timeout, [this](){ _UnLock(); return true; }) != 0)
            return -1;

        _Lock();
    }

    if (m_item_queue.empty()) {
        return -1;
    }

    item = m_item_queue.front();
    m_item_queue.pop();
    Assert(_OnEnableWrite() == 0);

    m_is_reading = false;
    _UnLock();

    return 0;
}

template<class TItem, int Max>
void Chan<TItem, Max>::Close()
{
    if (IsClosed())
        return;

    m_run_status = ChanStatus::CHAN_CLOSE;

    _Lock();
    /* 唤醒所有阻塞在Channel上的协程 */
    if (m_is_reading) _OnEnableRead();

    while (!m_enable_write_conds.empty()) {
        auto enable_write_cond = m_enable_write_conds.front();
        m_enable_write_conds.pop();
        enable_write_cond->Notify();
    }

    _UnLock();
}

template<class TItem, int Max>
bool Chan<TItem, Max>::IsClosed()
{
    return (m_run_status == ChanStatus::CHAN_CLOSE);
}

template<class TItem, int Max>
int Chan<TItem, Max>::_WaitUntilEnableRead(const detail::CoroutineOnYieldCallback& cb)
{
    return m_enable_read_cond->WaitWithCallback(cb);
}

template<class TItem, int Max>
int Chan<TItem, Max>::_WaitUntilEnableWrite(CoWaiter::SPtr cond, const detail::CoroutineOnYieldCallback& cb)
{
    return cond->WaitWithCallback(std::forward<const detail::CoroutineOnYieldCallback&>(cb));
}

template<class TItem, int Max>
int Chan<TItem, Max>::_WaitUntilEnableWriteOrTimeout(CoWaiter::SPtr cond, int timeout_ms, const detail::CoroutineOnYieldCallback& cb)
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
int Chan<TItem, Max>::_WaitUntilEnableReadOrTimeout(int timeout_ms, const detail::CoroutineOnYieldCallback& cb)
{
    return m_enable_read_cond->WaitWithTimeoutAndCallback(timeout_ms, cb);
}

template<class TItem, int Max>
void Chan<TItem, Max>::_Lock()
{
    m_item_queue_mutex.lock();
}

template<class TItem, int Max>
void Chan<TItem, Max>::_UnLock()
{
    m_item_queue_mutex.unlock();
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
    if (IsClosed())
        return -1;

    BaseType::_Lock();

    /* 加入引用队列，并尝试唤醒挂起的读端，如果没有正在读的协程挂起等待 */
    m_item_cache_ref.push(item);
    m_write_idx++;
    auto is_writing_idx = m_write_idx - 1;

    if (BaseType::m_is_reading && (is_writing_idx == m_read_idx)) {
        BaseType::_OnEnableRead();
        BaseType::_UnLock();
        return 0;
    }

    auto enable_write_cond = BaseType::_CreateAndPushEnableWriteCond();

    if (BaseType::_WaitUntilEnableWrite(enable_write_cond, [this](){ BaseType::_UnLock(); return true; }) != 0)
        return -1;
    
    return 0;
}

template<class TItem>
int Chan<TItem, 0>::Read(ItemType& item)
{
    // 如果信道关闭，不可以读了
    if (IsClosed())
        return -1;

    bool expect = false;
    if (!BaseType::m_is_reading.compare_exchange_strong(expect, true))
        return -1;

    BaseType::_Lock();

    if (m_write_idx <= m_read_idx) {
        if (BaseType::_WaitUntilEnableRead([this](){ BaseType::_UnLock(); return true; }) != 0)
            return -1;

        BaseType::_Lock();
    }
    
    item = m_item_cache_ref.front();
    m_item_cache_ref.pop();
    m_read_idx++;
    /* 抛出队列可写 */
    Assert(BaseType::_OnEnableWrite() == 0);

    BaseType::m_is_reading = false;
    BaseType::_UnLock();

    return 0;
}

template<class TItem>
void Chan<TItem, 0>::Close()
{
    if (IsClosed())
        return;

    BaseType::m_run_status = ChanStatus::CHAN_CLOSE;

    BaseType::_Lock();
    /* 唤醒所有阻塞在Channel上的协程 */
    if (BaseType::m_is_reading) BaseType::_OnEnableRead();

    while (!BaseType::m_enable_write_conds.empty()) {
        auto enable_write_cond = BaseType::m_enable_write_conds.front();
        BaseType::m_enable_write_conds.pop();
        enable_write_cond->Notify();
    }

    BaseType::_UnLock();
}

template<class TItem>
bool Chan<TItem, 0>::IsClosed()
{
    return (BaseType::m_run_status == ChanStatus::CHAN_CLOSE);
}

}