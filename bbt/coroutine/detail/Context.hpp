#pragma once
#include <functional>
#include <boost/context/detail/fcontext.hpp>
#include <bbt/base/assert/Assert.hpp>
#include <bbt/coroutine/detail/Stack.hpp>

namespace bbt::coroutine::detail
{

using fcontext_t = boost::context::detail::fcontext_t;

/**
 * @brief 协程上下文。提供了对栈可切换的上下文操作
 * 
 */
class Context
{
public:
    Context(size_t stack_size, const CoroutineCallback& co_func, const CoroutineFinalCallback& co_final, bool stack_protect = true);
    ~Context();

    /**
     * @brief 挂起（让出当前CPU控制权）
     */
    void                        Yield();

    void                        YieldWithCallback(const CoroutineOnYieldCallback& cb);

    /**
     * @brief 唤醒（切换到当前协程，给与CPU控制权）
     */
    void                        Resume();

    /**
     * @brief 获取当前线程正在运行的协程上下文
     * @return fcontext_t& 
     */
    static fcontext_t&          GetCurThreadContext();

protected:
    /**
     * @brief 执行协程
     * @param transfer
     */
    static void                 _CoroutineMain(boost::context::detail::transfer_t transfer);

    void                        _Yield();
    void                        _Resume();
private:
    fcontext_t                  m_context;
    CoroutineCallback           m_user_main{nullptr};
    CoroutineFinalCallback      m_final_handle{nullptr};
    CoroutineOnYieldCallback    m_onyield_callback{nullptr};
    Stack*                      m_stack{nullptr};                    

};

}