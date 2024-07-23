#pragma once
#include <bbt/base/assert/Assert.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>


namespace bbt::coroutine::sync
{

template<class TItem, uint32_t Max>
Chan<TItem, Max>::Chan(int max_queue_size):
    m_max_size(max_queue_size),
    m_enable_read_cond(CoCond::Create())
{
    Assert(m_max_size > 0);
    m_run_status = ChanStatus::CHAN_OPEN;
}

template<class TItem, uint32_t Max>
Chan<TItem, Max>::~Chan()
{
    if (!IsClosed())
        Close();
}

template<class TItem, uint32_t Max>
int Chan<TItem, Max>::Write(const ItemType& item)
{
    if (IsClosed())
        return -1;

    std::unique_lock<std::mutex> _(m_item_queue_mutex);
    if (m_item_queue.size() >= m_max_size)
        return -1;
    
    m_item_queue.push(item);
    if (m_is_reading) _Notify();

    return 0;
}

template<class TItem, uint32_t Max>
int Chan<TItem, Max>::Read(ItemType& item)
{
    // 如果信道关闭，不可以读了
    if (IsClosed())
        return -1;

    bool expect = false;
    if (!m_is_reading.compare_exchange_strong(expect, true))
        return -1;

    m_item_queue_mutex.lock();
    if (m_item_queue.empty()) {
        m_item_queue_mutex.unlock();
        // 这里释放锁是因为协程会挂起，如果不释放其他做写操作的协程没法写
        _Wait();    // 挂起，直到其他协程写
        m_item_queue_mutex.lock();
    }
    
    item = m_item_queue.front();
    m_item_queue.pop();
    m_is_reading = false;
    m_item_queue_mutex.unlock();

    return 0;
}


template<class TItem, uint32_t Max>
int Chan<TItem, Max>::ReadAll(std::vector<ItemType>& items)
{
    if (IsClosed())
        return -1;
    
    bool expect = false;
    if (!m_is_reading.compare_exchange_strong(expect, true))
        return -1;

    m_item_queue_mutex.lock();
    if (m_item_queue.empty()) {
        m_item_queue_mutex.unlock();
        _Wait();
        m_item_queue_mutex.lock();
    }

    while (!m_item_queue.empty()) {
        items.push_back(m_item_queue.front());
        m_item_queue.pop();
    }
    m_is_reading = false;
    m_item_queue_mutex.unlock();

    return 0;
}

template<class TItem, uint32_t Max>
int Chan<TItem, Max>::TryWrite(const ItemType& item)
{
    
}

template<class TItem, uint32_t Max>
int Chan<TItem, Max>::TryWrite(const ItemType& item, int timeout)
{

}

template<class TItem, uint32_t Max>
int Chan<TItem, Max>::TryRead(ItemType& item)
{
    if (IsClosed())
        return -1;
    
    bool expect = false;
    if (!m_is_reading.compare_exchange_strong(expect, true))
        return -1;

    std::unique_lock<std::mutex> _(m_item_queue_mutex);

    if (m_item_queue.empty())
        return -1;
    
    item = m_item_queue.front();
    m_item_queue.pop();

    return 0;
}

template<class TItem, uint32_t Max>
int Chan<TItem, Max>::TryRead(ItemType& item, int timeout)
{
    if (IsClosed())
        return -1;
    
    bool expect = false;
    if (!m_is_reading.compare_exchange_strong(expect, true))
        return -1;

    m_item_queue_mutex.lock();
    if (m_item_queue.empty()) {
        m_item_queue_mutex.unlock();
        _WaitWithTimeout(timeout);
        m_item_queue_mutex.lock();
    }

    if (m_item_queue.empty()) {
        m_item_queue_mutex.unlock();
        return -1;
    }

    item = m_item_queue.front();
    m_item_queue.pop();
    m_is_reading = false;
    m_item_queue_mutex.unlock();

    return 0;
}

template<class TItem, uint32_t Max>
void Chan<TItem, Max>::Close()
{
    if (IsClosed())
        return;
    
    if (m_is_reading) _Notify();
    m_run_status = ChanStatus::CHAN_CLOSE;
}

template<class TItem, uint32_t Max>
bool Chan<TItem, Max>::IsClosed()
{
    return (m_run_status == ChanStatus::CHAN_CLOSE);
}

template<class TItem, uint32_t Max>
int Chan<TItem, Max>::_Wait()
{
    return m_enable_read_cond->Wait();
}

template<class TItem, uint32_t Max>
int Chan<TItem, Max>::_Notify()
{
    return m_enable_read_cond->Notify();
}

template<class TItem, uint32_t Max>
int Chan<TItem, Max>::_WaitWithTimeout(int timeout_ms)
{
    return m_enable_read_cond->WaitWithTimeout(timeout_ms);
}

template<class TItem, uint32_t Max>
bool operator<<(Chan<TItem, Max>& chan, const typename Chan<TItem, Max>::ItemType& item)
{
    return (chan.Write(item) == 0);
}

template<class TItem, uint32_t Max>
bool operator>>(Chan<TItem, Max>& chan, typename Chan<TItem, Max>::ItemType& item)
{
    return (chan.Read(item) == 0);
}

template<class TItem, uint32_t Max>
bool operator<<(typename Chan<TItem, Max>::SPtr& chan, const TItem& item)
{
    return (chan->Write(item) == 0);
}

template<class TItem, uint32_t Max>
bool operator>>(typename Chan<TItem, Max>::SPtr& chan, TItem& item)
{
    return (chan->Read(item) == 0);
}

}