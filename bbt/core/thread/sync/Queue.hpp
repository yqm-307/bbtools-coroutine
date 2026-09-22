#pragma once
#include <boost/lockfree/queue.hpp>
#include <boost/noncopyable.hpp>

namespace bbt::core::thread
{


template <typename TValue, typename ...Options>
class Queue:
    public boost::noncopyable
{
public:
    Queue(size_t default_size): m_queue(default_size) {}
    Queue() = default;
    ~Queue() = default;

    bool Push(const TValue& cmd)
    {
        if (m_queue.push(cmd))
        {
            m_count++;
            return true;
        }
        return false;
    }

    bool Pop(TValue& cmd)
    {
        if (m_queue.pop(cmd))
        {
            m_count--;
            return true;
        }
        return false;
    }

    // 并非准确，返回参考值
    bool Empty() const
    {
        return m_queue.empty();
    }

    // 并非准确，返回参考值
    size_t Size() const
    {
        int size = m_count.load();
        return size < 0 ? 0 : size;
    }
private:
    boost::lockfree::queue<TValue, Options...> m_queue;
    std::atomic_size_t          m_count{0};
};
}