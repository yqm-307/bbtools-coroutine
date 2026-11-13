/**
 * @file __TQueue.hpp
 * @brief Queue 的模板实现，由 Queue.hpp 末尾包含。
 */
#pragma once
#include <bbt/core/thread/sync/Queue.hpp>

namespace bbt::core::thread {

template <typename TValue, typename... Options>
Queue<TValue, Options...>::Queue(size_t default_size) : m_queue(default_size)
{
}

template <typename TValue, typename... Options> bool Queue<TValue, Options...>::Push(const TValue& cmd)
{
    if (m_queue.push(cmd)) {
        m_count++;
        return true;
    }
    return false;
}

template <typename TValue, typename... Options> bool Queue<TValue, Options...>::Pop(TValue& cmd)
{
    if (m_queue.pop(cmd)) {
        m_count--;
        return true;
    }
    return false;
}

template <typename TValue, typename... Options> bool Queue<TValue, Options...>::Empty() const
{
    return m_queue.empty();
}

template <typename TValue, typename... Options> size_t Queue<TValue, Options...>::Size() const
{
    int size = m_count.load();
    return size < 0 ? 0 : size;
}

}
