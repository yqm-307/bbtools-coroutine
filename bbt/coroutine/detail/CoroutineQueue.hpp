#pragma once
#include <deque>
#include <bbt/coroutine/detail/Coroutine.hpp>

namespace bbt::coroutine::detail
{

class CoroutineQueue
{
public:
    CoroutineQueue();
    ~CoroutineQueue();

    bool                            Empty();
    size_t                          Size();
    Coroutine::SPtr                 PopHead();
    Coroutine::SPtr                 PopTail();
    void                            PushHead(Coroutine::SPtr);
    void                            PushTail(Coroutine::SPtr);
    void                            PopAll(std::vector<Coroutine::SPtr>& out);
    void                            PushHeadRange(std::vector<Coroutine::SPtr>::iterator begin, std::vector<Coroutine::SPtr>::iterator end);
    void                            PushTailRange(std::vector<Coroutine::SPtr>::iterator begin, std::vector<Coroutine::SPtr>::iterator end);
private:
    std::deque<Coroutine::SPtr>     m_queue;
    std::recursive_mutex            m_mutex;
};

}