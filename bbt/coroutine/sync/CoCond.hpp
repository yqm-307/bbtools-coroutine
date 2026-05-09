#pragma once
#include <bbt/core/thread/sync/Queue.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

namespace bbt::coroutine::sync
{

class CoCond:
    public std::enable_shared_from_this<CoCond>
{
    struct PrivateTag {};
public:
    typedef std::shared_ptr<CoCond> SPtr;

    static SPtr Create();

    BBTATTR_FUNC_CTOR_HIDDEN
    CoCond(PrivateTag);
    virtual ~CoCond();

    /**
     * @brief 使当前Coroutine挂起，直到被Notify（只能在Coroutine中使用）
     * 
     * @return int 是否成功挂起
     */
    int                         Wait();

    /**
     * @brief 使当前Coroutine挂起，直到被Notify或超时（只能在Coroutine中使用）
     * 
     * @param ms 超时时间
     * @return int 是否成功挂起
     */
    int                         WaitFor(int ms);

    /**
     * @brief 唤醒一个在此CoCond上挂起的Coroutine
     * 
     * @return int 如果有Coroutine被唤醒，返回0；否则返回-1
     */
    int                         NotifyOne();

    /**
     * @brief 唤醒所有在此CoCond上挂起的Coroutine
     * 
     */
    void                        NotifyAll();
protected:
    int                         _NotifyOne();
private:
    bbt::core::thread::Queue<CoWaiter*>         m_waiter_queue{8};
};

} // namespace bbt::coroutine::sync