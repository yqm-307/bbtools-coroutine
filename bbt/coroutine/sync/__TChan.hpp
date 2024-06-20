#pragma once
#include <bbt/coroutine/sync/Chan.hpp>
#include <bbt/base/assert/Assert.hpp>

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
}

int Chan::Write(const ItemType& item)
{
    if (IsClosed())
        return -1;

    std::unique_lock<std::mutex> _(m_item_queue_mutex);
    if (m_item_queue.size() >= m_max_size)
        return -1;
    
    m_item_queue.push(item);

    return 0;
}

int Chan::Read(ItemType& item)
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

int Chan::TryRead(ItemType& item)
{
    return -1;
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

}