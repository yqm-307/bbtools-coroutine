/**
 * @file ILock.hpp
 * @brief 线程锁的最小接口 IBasicLockable：只有 lock/unlock/try_lock。
 */
#pragma once
#include <pthread.h>
#include <atomic>
#include <thread>
#include <time.h>
#include <stdlib.h>

namespace bbt::core::thread {

/**
 * @brief 线程锁的最小约定：只有 lock/unlock/try_lock，递归性与公平性由实现声明（Mutex 递归，Spinlock 不）。
 * 签名满足 BasicLockable，可直接配 std::lock_guard。接口不禁止拷贝：Spinlock 未用 noncopyable 挡住，
 * 但锁状态按值持有，复制后不得当共享锁使用。
 */
class IBasicLockable {
public:
    /**
     * @brief 允许经基类指针销毁派生锁对象。
     */
    virtual ~IBasicLockable() = default;

    /**
     * @brief 取得锁；是否阻塞、能否同线程重入由实现决定。
     */
    virtual void lock() = 0;

    /**
     * @brief 释放锁；必须由持锁线程调用。
     */
    virtual void unlock() = 0;

    /**
     * @brief 非阻塞尝试：拿到返回 true，已被他人持有返回 false。
     */
    virtual bool try_lock() = 0;
};

}
