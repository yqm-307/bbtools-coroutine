/** @file Mutex.hpp
 * @brief 可递归的 pthread 互斥锁实现（IBasicLockable）。
 */
#pragma once
#include <bbt/core/thread/lock/ILock.hpp>
#include <boost/noncopyable.hpp>

namespace bbt::core::thread {

/** @brief 可递归的互斥锁：同一线程可重复 lock()，要同样次数 unlock() 才真正释放。
 *  递归由 PTHREAD_MUTEX_RECURSIVE 设定；unlock 多于 lock 时错误被丢弃（POSIX 下属未定义行为）。
 *  生命期必须覆盖所有会 lock 它的线程。
 */
class Mutex : public boost::noncopyable, public IBasicLockable {
public:
    /// @brief 构造可递归互斥锁（PTHREAD_MUTEX_RECURSIVE）。
    Mutex();
    /// @brief 析构：销毁底层 pthread 互斥量，属性对象不销毁。
    virtual ~Mutex();
    /// @brief 取得锁；同线程已持有时只增加递归计数，不阻塞。
    virtual void lock() override;

    /// @brief 释放一层递归；调用线程必须持有该锁。
    virtual void unlock() override;
    /// @brief 非阻塞尝试：拿到锁返回 true；递归锁下同线程重入也返回 true。
    virtual bool try_lock() override;
    /** @brief 借出底层 pthread_mutex_t，供 pthread_cond_wait/timedwait 配对使用（调用前须先 lock()）。
     *  等待期间 cond 通过它释放并重新取得锁；引用不能越过 Mutex 的生命期。
     */
    pthread_mutex_t& getlock();

private:
    pthread_mutexattr_t m_mutex_attr; ///< 只在构造时用来设定递归类型，析构不销毁
    pthread_mutex_t m_mutex;          ///< 底层互斥量；getlock() 把它借给 cond 等待
};

}
