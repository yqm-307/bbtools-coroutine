#pragma once
#include <bbt/coroutine/detail/Scheduler.hpp>


namespace bbt::coroutine
{

class _CoHelper
{
public:
    

    void operator-(const detail::CoroutineCallback& co_func)
    {
        g_scheduler->RegistCoroutineTask(co_func);
    }
};

}