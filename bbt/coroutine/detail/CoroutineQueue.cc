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
    return m_queue.empty();
}

size_t CoroutineQueue::Size()
{
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


}