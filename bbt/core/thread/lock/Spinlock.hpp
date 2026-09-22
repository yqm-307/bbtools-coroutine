#pragma once
#include <pthread.h>
#include <bbt/core/thread/lock/ILock.hpp>

namespace bbt::core::thread
{

class Spinlock:
    public IBasicLockable
{
public:
    Spinlock();
    virtual ~Spinlock();
    virtual void lock() override;
    virtual void unlock() override;
    virtual bool try_lock() override;

private:
    pthread_spinlock_t m_spin{-1};
};

}