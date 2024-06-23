#include <bbt/coroutine/detail/CoroutineQueue.hpp>

namespace bbt::coroutine::detail
{

CoroutineQueue::CoroutineQueue()
{
}

CoroutineQueue::~CoroutineQueue()
{
}

bool CoroutineQueue::Empty()
{
    std::lock_guard<std::mutex> _(m_mutex);
    return m_queue.empty();
}

size_t CoroutineQueue::Size()
{
    std::lock_guard<std::mutex> _(m_mutex);
    return m_queue.size();
}

Coroutine::SPtr CoroutineQueue::PopHead()
{
    std::lock_guard<std::mutex> _(m_mutex);
    auto coroutine_sptr = m_queue.front();
    m_queue.pop_front();
    return coroutine_sptr;
}

Coroutine::SPtr CoroutineQueue::PopTail()
{
    std::lock_guard<std::mutex> _(m_mutex);
    auto coroutine_sptr = m_queue.back();
    m_queue.pop_back();
    return coroutine_sptr;
}

void CoroutineQueue::PushHead(Coroutine::SPtr co)
{
    std::lock_guard<std::mutex> _(m_mutex);
    m_queue.push_front(co);
}

void CoroutineQueue::PushTail(Coroutine::SPtr co)
{
    std::lock_guard<std::mutex> _(m_mutex);
    m_queue.push_back(co);
}

void CoroutineQueue::PopAll(std::vector<Coroutine::SPtr>& out)
{
    std::lock_guard<std::mutex> _(m_mutex);
    for (auto&& item : m_queue)
    {
        out.push_back(item);
    }

    m_queue.clear();
}

void CoroutineQueue::PopNTail(std::vector<Coroutine::SPtr>& out, size_t n)
{
    std::lock_guard<std::mutex> _(m_mutex);
    size_t can_give_num = (m_queue.size() >= n) ? n : m_queue.size();
    for (int i = 0; i < can_give_num; ++i) {
        out.push_back(m_queue.back());
        m_queue.pop_back();
    }
}

void CoroutineQueue::PushHeadRange(std::vector<Coroutine::SPtr>::iterator begin, std::vector<Coroutine::SPtr>::iterator end)
{
    std::lock_guard<std::mutex> _(m_mutex);
    for (auto i = begin; i != end; i++)
        m_queue.push_front((*i));
}

void CoroutineQueue::PushTailRange(std::vector<Coroutine::SPtr>::iterator begin, std::vector<Coroutine::SPtr>::iterator end)
{
    std::lock_guard<std::mutex> _(m_mutex);
    for (auto i = begin; i != end; i++)
        m_queue.push_back((*i));
}

}