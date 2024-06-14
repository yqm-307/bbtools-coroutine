#include <deque>
#include <bbt/coroutine/detail/Coroutine.hpp>

namespace bbt::coroutine::detail
{

class CoroutineQueue
{
public:
    CoroutineQueue();

    ~CoroutineQueue();
private:
    std::deque<Coroutine> m_queue;
};

}