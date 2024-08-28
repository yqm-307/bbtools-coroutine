#include <bbt/coroutine/detail/CoroutineQueue.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>

namespace bbt::coroutine::detail
{

CoroutineQueue::CoroutineQueue(bool use_spinlock)
{
}

CoroutineQueue::~CoroutineQueue()
{
}

void CoroutineQueue::Clear()
{
    m_queue.clear();
}

bool CoroutineQueue::Empty()
{
    return m_queue.empty();
}

size_t CoroutineQueue::Size()
{
    return m_queue.size();
}

Coroutine::SPtr CoroutineQueue::PopHead()
{
    auto coroutine_sptr = m_queue.front();
    m_queue.pop_front();
    return coroutine_sptr;
}

Coroutine::SPtr CoroutineQueue::PopTail()
{
    auto coroutine_sptr = m_queue.back();
    m_queue.pop_back();
    return coroutine_sptr;
}

void CoroutineQueue::PushHead(Coroutine::SPtr co)
{
    m_queue.push_front(co);
}

void CoroutineQueue::PushTail(Coroutine::SPtr co)
{
    m_queue.push_back(co);
}

void CoroutineQueue::PopAll(std::vector<Coroutine::SPtr>& out)
{
    for (auto&& item : m_queue)
    {
        out.push_back(item);
    }

    m_queue.clear();
}

void CoroutineQueue::PopNTail(std::vector<Coroutine::SPtr>& out, size_t n)
{
    size_t can_give_num = (m_queue.size() >= n) ? n : m_queue.size();
    for (int i = 0; i < can_give_num; ++i) {
        out.emplace_back(m_queue.back());
        m_queue.pop_back();
    }
}

void CoroutineQueue::PopNHead(std::vector<Coroutine::SPtr>& out, size_t n)
{
    size_t can_give_num = (m_queue.size() >= n) ? n : m_queue.size();
    for (int i = 0; i < can_give_num; ++i) {
        out.emplace_back(m_queue.front());
        m_queue.pop_front();
    }
}

void CoroutineQueue::PushHeadRange(std::vector<Coroutine::SPtr>::iterator begin, std::vector<Coroutine::SPtr>::iterator end)
{
    for (auto i = begin; i != end; i++)
        m_queue.emplace_front((*i));
}

void CoroutineQueue::PushTailRange(std::vector<Coroutine::SPtr>::iterator begin, std::vector<Coroutine::SPtr>::iterator end)
{
    for (auto i = begin; i != end; i++)
        m_queue.emplace_back((*i));
}


}