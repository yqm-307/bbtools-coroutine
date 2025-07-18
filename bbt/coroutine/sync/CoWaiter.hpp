#pragma once
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::sync
{

/**
 * @brief 实现协程等待和唤醒的功能
 * 
 * 通过CoWaiter可以定制的去实现协程间的同步机制，事实上
 * bbtco中也是如此做的
 */
class CoWaiter
{
public:
    typedef std::shared_ptr<CoWaiter> SPtr;
    static SPtr                         Create();

    CoWaiter();
    ~CoWaiter();

    /**
     * @brief 挂起当前协程，直到被唤醒。如果有多个协程调用Wait族函数只有第一个成功
     *  其余调用者会失败。
     * @return 0表示被唤醒，-1表示失败
     */
    int                                 Wait();

    /**
     * @brief 挂起当前协程，知道被唤醒。如果有多个协程调用Wait族函数只有第一个成功
     *  其余调用者会失败。
     *  当参数cb为nullptr时，行为和Wait一致。和Wait同属Wait族函数
     * @param cb 当协程完成后调用此函数
     * @return 0表示被唤醒，-1表示失败
     */
    int                                 WaitWithCallback(const detail::CoroutineOnYieldCallback& cb);

    /**
     * @brief 挂起当前协程，直到被唤醒或者超时。如果有多个
     * 调用，只有第一个成功，其余调用者会失败
     * @param ms 
     * @return int 0表示触发事件，-1表示失败，1表示超时
     */
    int                                 WaitWithTimeout(int ms);

    /**
     * @brief 挂起当前协程，直到被唤醒或者超时。如果有多个
     * 调用，只有第一个成功，其余调用者失败
     * @param ms 
     * @param cb 
     * @return int 
     */
    int                                 WaitWithTimeoutAndCallback(int ms, const detail::CoroutineOnYieldCallback& cb);

    /**
     * @brief 唤醒一个因为调用Wait、WaitWithTimeout而挂起的协程
     *  ，如果没有携程因为Wait相关调用挂起，则Notify会失败
     * @return  0表示成功，-1表示事件已经触发
     */
    int                                 Notify();
protected:
    std::shared_ptr<detail::CoPollEvent> m_co_event{nullptr};
    volatile CoCondStatus               m_run_status{COND_DEFAULT};
};

}