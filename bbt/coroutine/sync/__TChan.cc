// #pragma once
#include <bbt/base/assert/Assert.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>


namespace bbt::coroutine::sync
{

Chan::Chan(int max_queue_size):
    m_max_size(max_queue_size)
{
    Assert(m_max_size > 0);
    m_run_status = ChanStatus::CHAN_OPEN;
}

Chan::~Chan()
{
    if (!IsClosed())
        Close();
}

int Chan::Write(const ItemType& item)
{
    if (IsClosed())
        return -1;

    std::unique_lock<std::mutex> _(m_item_queue_mutex);
    if (m_item_queue.size() >= m_max_size)
        return -1;
    
    m_item_queue.push(item);
    _Notify();

    return 0;
}

int Chan::Read(ItemType& item)
{
    if (IsClosed())
        return -1;
    
    bool a = false;
    if (!m_is_reading.compare_exchange_strong(a, true))
        return -1;

    m_item_queue_mutex.lock();
    if (m_item_queue.empty()) {
        m_item_queue_mutex.unlock();
        _Wait();
    }
    
    item = m_item_queue.front();
    m_item_queue.pop();
    m_item_queue_mutex.unlock();
    m_is_reading = false;

    return 0;
}

int Chan::TryRead(ItemType& item)
{
    if (!IsClosed())
        return -1;
    
    bool a = false;
    if (!m_is_reading.compare_exchange_strong(a, true))
        return -1;

    std::unique_lock<std::mutex> _(m_item_queue_mutex);
    if (m_item_queue.empty())
        return -1;
    
    item = m_item_queue.front();
    m_item_queue.pop();

    return 0;
}

int Chan::TryRead(ItemType& item, int timeout)
{
    return -1;
}

void Chan::Close()
{
    m_run_status = ChanStatus::CHAN_CLOSE;
}

bool Chan::IsClosed()
{
    return (m_run_status == ChanStatus::CHAN_CLOSE);
}

int Chan::_Wait()
{
    {
        std::unique_lock<std::mutex> _(m_wait_co_mutex);
        auto cur_co = g_bbt_tls_coroutine_co;
        if (cur_co == nullptr || m_wait_co != nullptr)
            return -1;

        m_wait_co = cur_co;
    }
    m_wait_co->Yield();
    return 0;
}

int Chan::_Notify()
{
    detail::Coroutine::SPtr wait_co = nullptr;

    {
        if (m_wait_co == nullptr)
            return -1;

        wait_co = m_wait_co;
        m_wait_co = nullptr;
    }
    wait_co->OnEventChanWrite();
    return 0;
}

}