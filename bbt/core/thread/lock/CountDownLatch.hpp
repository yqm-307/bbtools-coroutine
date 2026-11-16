/**
 * @file CountDownLatch.hpp
 * @brief 倒计数闩锁：计数降到 0 时唤醒全部等待者。
 */
#pragma once
#include <bbt/core/thread/lock/Mutex.hpp>

namespace bbt::core::thread {

/**
 * @brief 倒计数闩锁。Down() 递减计数，减到 <= 0 时 broadcast 唤醒全部等待者。
 * 计数与广播同在一把 Mutex 内完成，不会丢唤醒。Wait 阻塞调用线程的 OS 线程，在协程里调用会卡住整个
 * worker，不要用它等只由本 worker 推进的事件。
 */
class CountDownLatch : public boost::noncopyable {
public:
    /**
     * @brief 以初始计数 @p cot 构造；@p cot <= 0 时 Wait 立即返回。
     */
    CountDownLatch(int cot);

    /**
     * 须等所有 Wait/Down 返回后再析构，析构不唤醒在途等待者
     */
    ~CountDownLatch();

    /**
     * @brief 阻塞直到计数 <= 0。
     */
    void Wait();

    /**
     * @brief 等待被唤醒或超时（@p timeout 毫秒）。
     * @return 0 表示这次等待结束了——唤醒后不复查计数，spurious wakeup 也返回 0，不保证计数已归零；
     *         -1 表示超时。
     */
    int WaitTimeout(int timeout);

    /**
     * @brief 计数减一；减到 <= 0 时 broadcast 唤醒全部等待者。
     * 允许减成负数（多调 Down 不校验），之后 Wait 立即返回。
     */
    void Down();

    /**
     * @brief 直接复位计数，不加锁、不广播。
     * 只能在没有线程阻塞于 Wait 时调用：等待期间调用会让复查的等待者看到新的正计数而重新睡下，
     * 且收不到这次唤醒。
     */
    void Reset(int n);

private:
    /**
     * 剩余计数；Down 在锁内递减，Reset 例外，所以用 atomic
     */
    std::atomic_int m_count;

    /**
     * 保护计数与 m_sem 的"检查-等待"/"改计数-广播"配对
     */
    Mutex m_lock;

    /**
     * 计数归零时 broadcast，等待时配合 m_lock.getlock()
     */
    pthread_cond_t m_sem;
};

}
