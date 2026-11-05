#pragma once
#include <bbt/coroutine/detail/Stack.hpp>
#include <boost/context/detail/fcontext.hpp>

/**
 * @file ICoroutine.hpp
 * @brief 协程对象对外的稳定 C++ 表面：恢复、让出与身份/状态查询。
 */

namespace bbt::coroutine::detail {

/** @brief 协程对象上允许外部调用的最小集合；Create 是 Coroutine 的静态入口，RequestCancel / GetException 见 Coroutine.hpp。 */
class ICoroutine {
public:
    /* 切换到协程上下文执行，直到它让出或结束 */
    virtual void Resume() = 0;
    /* 让出：保存上下文，回到 Resume 的调用点 */
    virtual void Yield() = 0;
    /* 挂起成功后再执行 cb；用于「先挂起、再同步触发事件」的场景。
     * 成功返回 0，cb 返回 false 时返回 -1。 */
    virtual int YieldWithCallback(const CoroutineOnYieldCallback& cb) = 0;

    /* 返回创建时分配的进程内唯一 id，单调递增、不复用 */
    virtual CoroutineId GetId() const noexcept = 0;
    /* 返回当前运行状态；读的是普通成员，不是跨线程同步点 */
    virtual CoroutineStatus GetStatus() const noexcept = 0;
};

}
