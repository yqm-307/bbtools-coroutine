#include <bbt/coroutine/pool/Work.hpp>
#include <bbt/coroutine/pool/CoPool.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/syntax/SyntaxMacro.hpp>


namespace bbt::coroutine::pool
{

std::shared_ptr<CoPool> CoPool::Create(int max_co)
{
    return std::make_shared<CoPool>(max_co);
}

CoPool::CoPool(int max):
    m_max_co_num(max),
    m_cond(sync::CoCond::Create()),
    m_latch(m_max_co_num + 1) // monitor co + work co
{
    Assert(m_max_co_num > 0);
    _Start();
}

CoPool::~CoPool()
{
    Release();

    Work* item = nullptr;
    while (m_works_queue.try_dequeue(item))
        delete item;
}

int CoPool::Submit(const CoPoolWorkCallback& workfunc)
{
    return _SubmitImpl(new Work(workfunc));
}

int CoPool::Submit(CoPoolWorkCallback&& workfunc)
{
    return _SubmitImpl(new Work(std::forward<CoPoolWorkCallback>(workfunc)));
}

void CoPool::Release()
{
    {
        std::lock_guard<std::mutex> lock(m_cond_mtx);
        if (!m_is_running.exchange(false))
            return;
    }

    while (m_running_co_num != 0 && g_scheduler->IsRunning()) {
        m_cond->NotifyAll();

        if (g_bbt_tls_helper->EnableUseCo())
            bbtco_sleep(5);
        else
            std::this_thread::sleep_for(bbt::core::clock::ms(5));
    } 

    if (g_scheduler->IsRunning()) {
        m_latch.Wait();
    }
}

int CoPool::_SubmitImpl(Work* work)
{
    {
        std::lock_guard<std::mutex> lock(m_cond_mtx);
        if (!m_is_running) {
            delete work;
            return -1;
        }

        BBTATTR_COMM_UNUSED auto enqueue_ok = m_works_queue.enqueue(work);
        AssertWithInfo(enqueue_ok, "oom!");
    }

    m_cond->NotifyOne();
    return 0;
}

void CoPool::_Start()
{
    /* 启动工作协程 */
    for (int i = 0; i < m_max_co_num; ++i)
        bbtco_desc("work co") [=](){
            _WorkCo();
            m_latch.Down();
        };

    /* 启动监视协程 */
    bbtco_desc("monitor co") [=](){
        _MonitorCo();
        m_latch.Down();
    };
}

void CoPool::_WorkCo()
{
    m_running_co_num++;

    while (true) {
        Work* work = nullptr;
        if (m_works_queue.try_dequeue(work)) {
            work->Invoke();
            delete work;
            continue;
        }

        if (!m_is_running)
            break;

        m_cond->Wait();
    }

    m_running_co_num--;
}

void CoPool::_MonitorCo()
{
    /**
     * 每隔一段时间，监视协程会检测是否有“摸鱼”的work协程，并将其
     * 唤醒起来执行任务
     */
    m_running_co_num++;

    while (m_is_running) {
        
        auto remain_works_num = m_works_queue.size_approx();

        for (int i = 0; i < remain_works_num; ++i) {
            /* 唤醒失败，说明没有阻塞的co */
            if (m_cond->NotifyOne() != 0)
                break;
        }

        bbtco_sleep(1);
    }

    m_running_co_num--;
}

}
