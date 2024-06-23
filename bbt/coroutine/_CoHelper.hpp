#include <bbt/coroutine/detail/Scheduler.hpp>


namespace bbt::coroutine
{

class _CoHelper
{
public:
    

    detail::CoroutineId operator-(const detail::CoroutineCallback& co_func)
    {
        return g_scheduler->RegistCoroutineTask(co_func);
    }
};

}