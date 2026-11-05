/** @file _CoHelper.hpp @brief bbtco / bbtco_noexcept / bbtco_desc 三个宏的运算符帮手。 */
#pragma once
#include <bbt/coroutine/detail/Scheduler.hpp>

namespace bbt::coroutine {

/**
 * @brief 启动协程宏的左操作数：`_CoHelper(...) - cb` 转调 RegistCoroutineTask，noexcept 版用 operator+ 写回 bool。
 *        对象只在整条表达式内活着；被保存的是拷贝进协程的 cb 和 desc 指针，desc 须活到协程结束。
 */
class _CoHelper {
public:
    /// @brief 记录注册结果的出口：succ 由调用者持有，operator+ 注册后写它（true 成功 / false 失败）
    _CoHelper(bool* succ = nullptr) : m_regist_succ(succ)
    {
    }

    /// @brief bbtco_desc 携带任务描述：落库到协程，诊断现场可读回。指针只借用，不复制字符串
    explicit _CoHelper(const char* desc) : m_desc(desc)
    {
    }

    /// @brief 注册协程；注册失败（无 worker、栈申请失败等）抛异常，调用点不会留下半注册的协程
    void operator-(const detail::CoroutineCallback& co_func)
    {
        g_scheduler->RegistCoroutineTask(co_func, m_desc);
    }

    /**
     * @brief 注册协程并把结果写回 bool；noexcept，异常被吞成 false。只有 bbtco_noexcept(&ok) 走这条，
     *        m_regist_succ 由宏参数保证非空（默认构造后直接调它会解引用空指针）。
     */
    void operator+(const detail::CoroutineCallback& co_func)
    {
        g_scheduler->RegistCoroutineTask(co_func, *m_regist_succ);
    }

private:
    bool* m_regist_succ{nullptr}; // 用于记录注册协程是否成功
    const char* m_desc{nullptr};
};

}
