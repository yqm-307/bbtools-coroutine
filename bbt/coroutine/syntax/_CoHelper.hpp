#pragma once
#include <string>
#include <bbt/coroutine/detail/Scheduler.hpp>


namespace bbt::coroutine
{

class _CoHelper
{
public:
    _CoHelper(bool* succ = nullptr, std::string desc = ""):
        m_regist_succ(succ),
        m_desc(std::move(desc))
    {
    }

    void operator-(const detail::CoroutineCallback& co_func)
    {
        g_scheduler->RegistCoroutineTask(co_func, m_desc);
    }

    void operator+(const detail::CoroutineCallback& co_func)
    {
        g_scheduler->RegistCoroutineTask(co_func, *m_regist_succ, m_desc);
    }
private:
    bool* m_regist_succ{nullptr}; // 用于记录注册协程是否成功
    std::string m_desc;
};

}
