#include <bbt/core/thread/lock/CountDownLatch.hpp>
#include <mutex>

namespace bbt::core::thread
{

CountDownLatch::CountDownLatch(int cot):m_count(cot)
{
    m_sem = PTHREAD_COND_INITIALIZER;
}


CountDownLatch::~CountDownLatch()
{
}

void CountDownLatch::Wait()
{
    std::lock_guard<Mutex> lock(m_lock);
    // while 而非 if：pthread_cond_wait 允许 spurious wakeup，
    // 且 broadcast 后可能被其他等待者先消耗资源，需重新检查计数。
    while(m_count > 0)
        pthread_cond_wait(&m_sem,&m_lock.getlock());
}

int CountDownLatch::WaitTimeout(int timeout)
{
    std::lock_guard<Mutex> lock(m_lock);
    if (m_count > 0) {
        timespec now;
        timespec end_tm;
        clock_gettime(CLOCK_REALTIME, &now);
        int64_t total_nsec = now.tv_sec * 1000 * 1000 * 1000 + now.tv_nsec;
        // 截止时间 = now + timeout（毫秒）。此前实现漏加 timeout，导致
        // count>0 时 pthread_cond_timedwait 恒立即 ETIMEDOUT，语义等同
        // "立即超时"，所有带超时等待均失效。
        total_nsec += static_cast<int64_t>(timeout) * 1000 * 1000;
        end_tm.tv_sec = total_nsec / (1000 * 1000 * 1000);
        end_tm.tv_nsec = total_nsec - (end_tm.tv_sec * 1000 * 1000 * 1000);

        // 处理 EINTR：信号中断不视为超时，重算剩余时间继续等待
        while (true) {
            int ret = pthread_cond_timedwait(&m_sem, &m_lock.getlock(), &end_tm);
            if (ret == 0)
                return 0;
            if (ret == ETIMEDOUT)
                return -1;
            if (ret != EINTR)
                return -1;
            // EINTR：重新获取当前时间，若已过截止时间则超时
            timespec cur;
            clock_gettime(CLOCK_REALTIME, &cur);
            if (cur.tv_sec > end_tm.tv_sec ||
                (cur.tv_sec == end_tm.tv_sec && cur.tv_nsec >= end_tm.tv_nsec))
                return -1;
        }
    }

    return 0;
}


void CountDownLatch::Down()
{
    // 与 Wait/WaitTimeout 使用同一把锁：避免 count 已归零但 broadcast
    // 发生在 wait 原子进入之前导致的丢失唤醒（永久挂起）。
    std::lock_guard<Mutex> lock(m_lock);
    --m_count;
    if(m_count <= 0)
        pthread_cond_broadcast(&m_sem);
}

void CountDownLatch::Reset(int n)
{
    m_count = n;
}

}