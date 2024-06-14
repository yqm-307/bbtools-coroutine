#include <bbt/coroutine/detail/Coroutine.hpp>

namespace bbt::coroutine::detail
{

class Scheduler
{
public:
    Scheduler();
    ~Scheduler();
    void Start();
    void Stop();

    void RegistCoroutineTask(const CoroutineCallback& handle);
    void UnRegistCoroutineTask();
};

}