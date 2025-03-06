#pragma once
#include <deque>
#include <bbt/core/thread/lock/Spinlock.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>

namespace bbt::coroutine::detail
{

class CoroutineQueue
{
public:
    CoroutineQueue();
    ~CoroutineQueue();

    void                            Clear();
    bool                            Empty();
    size_t                          Size();
    Coroutine::SPtr                 PopHead();
    Coroutine::SPtr                 PopTail();
    void                            PushHead(Coroutine::SPtr);
    void                            PushTail(Coroutine::SPtr);
    void                            PopAll(std::vector<Coroutine::SPtr>& out);
    void                            PopNTail(std::vector<Coroutine::SPtr>& out, size_t n);
    void                            PopNHead(std::vector<Coroutine::SPtr>& out, size_t n);
    void                            PushHeadRange(std::vector<Coroutine::SPtr>::iterator begin, std::vector<Coroutine::SPtr>::iterator end);
    void                            PushTailRange(std::vector<Coroutine::SPtr>::iterator begin, std::vector<Coroutine::SPtr>::iterator end);
private:
    std::deque<Coroutine::SPtr>     m_queue;
};

}