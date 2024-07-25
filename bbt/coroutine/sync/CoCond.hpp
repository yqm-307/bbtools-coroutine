#pragma once
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/base/Attribute.hpp>

namespace bbt::coroutine::sync
{

class CoCond:
    public std::enable_shared_from_this<CoCond>
{
public:
    typedef std::shared_ptr<CoCond> SPtr;
    static SPtr                         Create();

    BBTATTR_FUNC_Ctor_Hidden            CoCond();
                                        ~CoCond();

    /**
     * @brief 挂起当前协程，直到被唤醒。如果有多个协程调用Wait只有第一个成功
     *  其余调用者会失败。
     * @return 0表示被唤醒，-1表示失败
     */
    int                                 Wait();

    /**
     * @brief 挂起当前协程，直到被唤醒或者超时。如果有多个
     * 调用，只有第一个成功，其余调用者会失败
     * @param ms 
     * @return int 0表示触发事件，-1表示失败，1表示超时
     */
    int                                 WaitWithTimeout(int ms);

    /**
     * @brief 唤醒一个因为调用Wait、WaitWithTimeout而挂起的协程
     *  ，如果没有携程因为Wait相关调用挂起，则Notify会失败
     * @return  0表示成功，-1表示事件已经触发
     */
    int                                 Notify();
protected:
    int                                 Init();
protected:
    std::shared_ptr<detail::CoPollEvent> m_co_event{nullptr};
    int                                 m_await_co_num{};
    std::mutex                          m_co_event_mutex;
    volatile CoCondStatus               m_run_status{COND_DEFAULT};
};

}