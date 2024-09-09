#pragma once
#include <bbt/base/thread/Lock.hpp>
#include <bbt/base/templateutil/Noncopyable.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/utils/lockfree/concurrentqueue.h>


namespace bbt::coroutine::pool
{

class CoPool:
    private bbt::templateutil::noncopyable
{
public:
    CoPool(int co_max);
    virtual ~CoPool();

    virtual int             Submit(const CoPoolWorkCallback& workfunc);
    virtual int             Submit(CoPoolWorkCallback&& workfunc);
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

    bbt::thread::CountDownLatch m_latch;

};

};
