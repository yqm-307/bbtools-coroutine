#include <deque>
#include <bbt/coroutine/detail/Coroutine.hpp>

namespace bbt::coroutine::detail
{

class CoroutineQueue
{
public:
    CoroutineQueue();
    ~CoroutineQueue();

    bool            Empty();
    size_t          Size();
    Coroutine::SPtr PopHead();
    Coroutine::SPtr PopTail();
    void            PushHead(Coroutine::SPtr);
    void            PushTail(Coroutine::SPtr);
private:
    std::deque<Coroutine::SPtr> m_queue;
    std::mutex                  m_mutex;
};

}