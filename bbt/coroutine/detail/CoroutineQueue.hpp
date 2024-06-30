#pragma once
#include <deque>
#include <bbt/base/thread/lock/Spinlock.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>

namespace bbt::coroutine::detail
{

class CoroutineQueue
{
public:
    CoroutineQueue(bool use_spinlock = false);
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
    // void                            PopNTail(CoroutineQueue& out, size_t n);
    void                            PushHeadRange(std::vector<Coroutine::SPtr>::iterator begin, std::vector<Coroutine::SPtr>::iterator end);
    void                            PushTailRange(std::vector<Coroutine::SPtr>::iterator begin, std::vector<Coroutine::SPtr>::iterator end);
    bool                            Exist(Coroutine::SPtr co);
protected:
    void                            Lock();
    void                            UnLock();
private:
    std::deque<Coroutine::SPtr>     m_queue;
    std::mutex                      m_mutex;
    bbt::thread::Spinlock           m_spinlock;
    const bool                      m_use_spinlock{false};
};

}