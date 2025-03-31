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
    m_enable_read_cond(CoWaiter::Create())
{
    static_assert(Max >= 0);
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

    /* 队列满了，写入失败。挂起并再次尝试写入 */
    while (!m_item_queue.Push(item)) {
        sync::CoWaiter co_writeable_waiter;
        if (co_writeable_waiter.WaitWithCallback([&]()
        {
            AssertWithInfo(m_enable_write_conds.Push(&co_writeable_waiter), "oom");
            return true;
        }) != 0)
            return -1;
    }

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

    // 保证作为读者的Coroutine只有一个
    bool expect = false;
    if (!m_is_reading.compare_exchange_strong(expect, true))
        return -1;

    /* 不断尝试pop出一个item，如果队列为空，就挂起直到有push操作来唤醒当前Coroutine */
    if (!m_item_queue.Pop(item)) {
        if (m_enable_read_cond->Wait() != 0)
            return -1;
        
        Assert(m_item_queue.Pop(item));
    }

    /* 可写，去唤醒任意一个等待push的Coroutine */
    Assert(_OnEnableWrite() == 0);
    m_is_reading.exchange(false);

    return 0;
}

template<class TItem, int Max>
int Chan<TItem, Max>::TryWrite(const ItemType& item)
{
    if (IsClosed())
        return -1;

    if (!m_item_queue.Push(item))
        return -1;
    
    if (m_is_reading) _OnEnableRead();

    return 0;
}

template<class TItem, int Max>
int Chan<TItem, Max>::TryWrite(const ItemType& item, int timeout)
{
    int ret = 0;

    if (IsClosed())
        return -1;

    if (!m_item_queue.Push(item)) {
        sync::CoWaiter co_writeable_waiter;
        if (co_writeable_waiter.WaitWithTimeoutAndCallback(timeout, [&]()
        {
            AssertWithInfo(m_enable_write_conds.Push(&co_writeable_waiter), "oom");
            return true;
        }) != 0)
            return -1;

        // 超时或者可写
        if (!m_item_queue.Push(item))
            return -1;
    }

    if (m_is_reading)
        _OnEnableRead();

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

    if (!m_item_queue.Pop(item))
        return -1;
    
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

    if (!m_item_queue.Pop(item)) {

        if (m_enable_read_cond->WaitWithTimeout(timeout) != 0)
            return -1;

        Assert(m_item_queue.Pop(item));
    }

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

    /* 唤醒所有阻塞在Channel上的协程 */
    if (m_is_reading) _OnEnableRead();

    CoWaiter* waiter = nullptr; 

    while (m_enable_write_conds.Pop(waiter)) {
        waiter->Notify();
    }
}

template<class TItem, int Max>
bool Chan<TItem, Max>::IsClosed()
{
    return (m_run_status == ChanStatus::CHAN_CLOSE);
}

template<class TItem, int Max>
int Chan<TItem, Max>::_OnEnableRead()
{
    return m_enable_read_cond->Notify();
}

template<class TItem, int Max>
int Chan<TItem, Max>::_OnEnableWrite()
{
    CoWaiter* waiter = nullptr;
    if (!m_enable_write_conds.Pop(waiter))
        return -1;

    return waiter->Notify();
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

    /* 加入引用队列，并尝试唤醒挂起的读端，如果没有正在读的协程挂起等待 */
    CoWaiter waiter;

    if (waiter.WaitWithCallback([&](){
        Assert(this->m_enable_write_conds.Push(&waiter));
        if (this->m_is_reading)
            BaseType::_OnEnableRead();

        return true;
    }) != 0)
        return -1;
    
    // 同时只能有一个Writer走到这里，醒来了就可以通知取数据了
    Assert(item == nullptr);
    m_item_ref = (intptr_t)&item;
    if (this->m_is_reading)
        BaseType::_OnEnableRead();

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

    int ret = this->m_enable_read_cond.Wait();

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