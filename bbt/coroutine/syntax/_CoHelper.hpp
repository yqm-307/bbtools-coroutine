#pragma once
#include <bbt/coroutine/detail/Scheduler.hpp>


namespace bbt::coroutine
{

class _CoHelper
{
public:
    _CoHelper(bool* succ = nullptr):
        m_regist_succ(succ)
    {
    }

    void operator-(const detail::CoroutineCallback& co_func)
    {
        g_scheduler->RegistCoroutineTask(co_func);
    }

    void operator+(const detail::CoroutineCallback& co_func)
    {
        g_scheduler->RegistCoroutineTask(co_func, *m_regist_succ);
    }
private:
    bool* m_regist_succ{nullptr}; // 用于记录注册协程是否成功
};

}