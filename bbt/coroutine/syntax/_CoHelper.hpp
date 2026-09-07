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

    /* bbtco_desc 携带任务描述（#276）：落库到协程，诊断现场可读回 */
    explicit _CoHelper(const char* desc):
        m_desc(desc)
    {
    }

    void operator-(const detail::CoroutineCallback& co_func)
    {
        g_scheduler->RegistCoroutineTask(co_func, m_desc);
    }

    void operator+(const detail::CoroutineCallback& co_func)
    {
        g_scheduler->RegistCoroutineTask(co_func, *m_regist_succ);
    }
private:
    bool* m_regist_succ{nullptr}; // 用于记录注册协程是否成功
    const char* m_desc{nullptr};
};

}