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
    m_max_size(max_queue_size),
    m_cond(CoCond::Create())
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
    // 如果信道关闭，不可以读了
    if (IsClosed())
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
    m_item_queue_mutex.unlock();

    return 0;
}

int Chan::TryRead(ItemType& item)
{
    if (IsClosed())
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
    if (IsClosed())
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
    m_item_queue_mutex.unlock();

    return 0;
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
    return m_cond->Wait();
}

int Chan::_Notify()
{
    return m_cond->Notify();
}

int Chan::_WaitWithTimeout(int timeout_ms)
{
    return m_cond->WaitWithTimeout(timeout_ms);
}

}