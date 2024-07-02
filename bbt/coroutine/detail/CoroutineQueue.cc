#include <bbt/coroutine/detail/CoroutineQueue.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>

namespace bbt::coroutine::detail
{

CoroutineQueue::CoroutineQueue(bool use_spinlock):
    m_use_spinlock(use_spinlock)
{
}

CoroutineQueue::~CoroutineQueue()
{
}

void CoroutineQueue::Clear()
{
    Lock();
    m_queue.clear();
    UnLock();
}

bool CoroutineQueue::Empty()
{
    Lock();
    bool is_empty = m_queue.empty();
    UnLock();
    return is_empty;
}

size_t CoroutineQueue::Size()
{
    Lock();
    size_t queue_size = m_queue.size();
    UnLock();
    return queue_size;
}

Coroutine::SPtr CoroutineQueue::PopHead()
{
    Lock();
    auto coroutine_sptr = m_queue.front();
    m_queue.pop_front();
    UnLock();
    return coroutine_sptr;
}

Coroutine::SPtr CoroutineQueue::PopTail()
{
    Lock();
    auto coroutine_sptr = m_queue.back();
    m_queue.pop_back();
    UnLock();
    return coroutine_sptr;
}

void CoroutineQueue::PushHead(Coroutine::SPtr co)
{
    Lock();
    m_queue.push_front(co);
    UnLock();
}

void CoroutineQueue::PushTail(Coroutine::SPtr co)
{
    Lock();
    m_queue.push_back(co);
    UnLock();
}

void CoroutineQueue::PopAll(std::vector<Coroutine::SPtr>& out)
{
    Lock();
    for (auto&& item : m_queue)
    {
        out.push_back(item);
    }

    m_queue.clear();
    UnLock();
}

void CoroutineQueue::PopNTail(std::vector<Coroutine::SPtr>& out, size_t n)
{
    Lock();
    size_t can_give_num = (m_queue.size() >= n) ? n : m_queue.size();
    for (int i = 0; i < can_give_num; ++i) {
        out.emplace_back(m_queue.back());
        m_queue.pop_back();
    }
    UnLock();
}

void CoroutineQueue::PushHeadRange(std::vector<Coroutine::SPtr>::iterator begin, std::vector<Coroutine::SPtr>::iterator end)
{
    Lock();
    for (auto i = begin; i != end; i++)
        m_queue.emplace_front((*i));
    UnLock();
}

void CoroutineQueue::PushTailRange(std::vector<Coroutine::SPtr>::iterator begin, std::vector<Coroutine::SPtr>::iterator end)
{
    Lock();
    for (auto i = begin; i != end; i++)
        m_queue.emplace_back((*i));
    UnLock();
}


void CoroutineQueue::Lock()
{
    if (m_use_spinlock)
        m_spinlock.Lock();
    else
        m_mutex.lock();
}

void CoroutineQueue::UnLock()
{
    if (m_use_spinlock)
        m_spinlock.UnLock();
    else
        m_mutex.unlock();
}

bool CoroutineQueue::Exist(Coroutine::SPtr co)
{
    for (auto&& it : m_queue)
        if (co == it) return true;
    
    return false;
}


}