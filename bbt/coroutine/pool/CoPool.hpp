#pragma once
#include <boost/noncopyable.hpp>
#include <bbt/core/thread/Lock.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/utils/lockfree/concurrentqueue.h>


namespace bbt::coroutine::pool
{

/**
 * @brief 协程池，池子会创建固定数量的协程来处理任务
 * 
 * 注意：
 * 1. 协程池的协程数量是固定的，不能动态增加
 * 2. 在协程池中执行任务，如过执行阻塞操作导致协程挂起，会占用这个协程
 *   导致池子中可用协程数变少
 * 3. 协程池中的协程会一直存在，直到协程池被销毁
 * 
 * todo：不是异常安全的，没有对oom进行处理
 */
class CoPool:
    boost::noncopyable
{
public:
    static std::shared_ptr<CoPool> Create(int max_co);

    BBTATTR_FUNC_CTOR_HIDDEN
    explicit CoPool(int co_max);
    virtual ~CoPool();

    virtual int             Submit(const CoPoolWorkCallback& workfunc);
    virtual int             Submit(CoPoolWorkCallback&& workfunc);

    /**
     * @brief 阻塞直到，池中所有协程全部退出
     */
    virtual void            Release();
protected:
    void                    _Start();
    /* 工作协程 */
    void                    _WorkCo();
    /* 监控协程 */
    void                    _MonitorCo();
private:
    volatile bool           m_is_running{true};
    const int               m_max_co_num{0};
    moodycamel::ConcurrentQueue<Work*> 
                            m_works_queue;
    std::atomic_int         m_running_co_num{0};

    std::shared_ptr<sync::CoCond>
                            m_cond{nullptr};
    std::mutex              m_cond_mtx;

    bbt::core::thread::CountDownLatch m_latch;

};

};
