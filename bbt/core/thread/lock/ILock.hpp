#pragma once
#include <pthread.h>
#include <atomic>
#include <thread>
#include <time.h>
#include <stdlib.h>

namespace bbt::core::thread
{

class IBasicLockable
{
public:
    virtual ~IBasicLockable() = default;

    virtual void lock() = 0;
    virtual void unlock() = 0;
    virtual bool try_lock() = 0;
};

}