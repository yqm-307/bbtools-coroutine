/**
 * @file Spinlock.hpp
 * @brief pthread 自旋锁实现（IBasicLockable），只适合保护极短的临界区。
 */
#pragma once
#include <pthread.h>
#include <bbt/core/thread/lock/ILock.hpp>

namespace bbt::core::thread {

/**
 * @brief 自旋锁：取不到锁时在用户态空转，只适合极短临界区。
 * 不可递归（同线程重入是未定义行为），持锁期间不能阻塞（IO、等条件变量、让出协程）。init/destroy 失败
 * 只经 Assert 记录，而 Assert 在 NDEBUG 构建里只打印不终止，所以失败后会继续用未初始化的锁。
 */
class Spinlock : public IBasicLockable {
public:
    /**
     * @brief 构造并初始化自旋锁；初始化失败只经 Assert 记录。
     */
    Spinlock();

    /**
     * @brief 析构：pthread_spin_destroy 并用 Assert 检查返回值；销毁前须确保无线程持有或访问该锁。
     */
    virtual ~Spinlock();

    /**
     * @brief 忙等直到拿到锁；同线程重入是未定义行为。
     */
    virtual void lock() override;

    /**
     * @brief 释放锁；调用线程必须持有该锁。
     */
    virtual void unlock() override;

    /**
     * @brief 单次尝试，不等待：拿到返回 true。
     */
    virtual bool try_lock() override;

private:
    /**
     * -1 只是占位初值，构造时由 pthread_spin_init 覆盖
     */
    pthread_spinlock_t m_spin{-1};
};

}
