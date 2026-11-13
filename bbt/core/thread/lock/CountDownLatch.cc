#include <bbt/core/thread/lock/CountDownLatch.hpp>
#include <mutex>

namespace bbt::core::thread {

CountDownLatch::CountDownLatch(int cot) : m_count(cot)
{
    m_sem = PTHREAD_COND_INITIALIZER;
}

CountDownLatch::~CountDownLatch()
{
    /**
     * m_sem 由 PTHREAD_COND_INITIALIZER 静态初始化，无配套 destroy；析构时仍有线程在 Wait 属调用方违约。
     */
}

void CountDownLatch::Wait()
{
    std::lock_guard<Mutex> lock(m_lock);
    /**
     * while 而非 if：spurious wakeup 醒来时 m_count 未必已归零，必须复查。
     */
    while (m_count > 0)
        pthread_cond_wait(&m_sem, &m_lock.getlock());
}

int CountDownLatch::WaitTimeout(int timeout)
{
    std::lock_guard<Mutex> lock(m_lock);
    if (m_count > 0) {
        timespec now;
        timespec end_tm;
        clock_gettime(CLOCK_REALTIME, &now);
        int64_t total_nsec = now.tv_sec * 1000 * 1000 * 1000 + now.tv_nsec;
        /**
         * timedwait 用绝对时间（CLOCK_REALTIME）：截止时间必须是 now + timeout，否则等于立即超时。
         */
        total_nsec += static_cast<int64_t>(timeout) * 1000 * 1000;
        end_tm.tv_sec = total_nsec / (1000 * 1000 * 1000);
        end_tm.tv_nsec = total_nsec - (end_tm.tv_sec * 1000 * 1000 * 1000);

        /**
         * 处理 EINTR：信号中断不视为超时，用同一个绝对截止时间重试，不需要重算剩余时间
         */
        while (true) {
            int ret = pthread_cond_timedwait(&m_sem, &m_lock.getlock(), &end_tm);
            if (ret == 0)
                return 0;
            if (ret == ETIMEDOUT)
                return -1;
            if (ret != EINTR)
                return -1;
            /**
             * EINTR：重新获取当前时间，若已过截止时间则超时
             */
            timespec cur;
            clock_gettime(CLOCK_REALTIME, &cur);
            if (cur.tv_sec > end_tm.tv_sec || (cur.tv_sec == end_tm.tv_sec && cur.tv_nsec >= end_tm.tv_nsec))
                return -1;
        }
    }

    return 0;
}

void CountDownLatch::Down()
{
    /**
     * 与 Wait 同一把锁：否则广播可能落在等待者进入 cond_wait 之前，通知丢失后永久挂起。
     */
    std::lock_guard<Mutex> lock(m_lock);
    --m_count;
    if (m_count <= 0)
        pthread_cond_broadcast(&m_sem);
}

void CountDownLatch::Reset(int n)
{
    /**
     * 不持锁：只能在没有并发 Wait/WaitTimeout/Down 时调用（典型用法是上一轮等待全部返回后复用）。
     */
    m_count = n;
}

}
