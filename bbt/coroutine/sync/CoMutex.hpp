#pragma once
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>

namespace bbt::coroutine::sync
{

/**
 * 用来实现协程间的同步
 * 
 * 当多个协程有数据竞争时，使用Mutex可以让发生竞争的锁
 * 挂起等待，当有获取锁的协程解锁后，将唤醒一个竞争中的
 * 协程。
 * 
 * 使用此锁是为了防止阻塞系统线程
 */
class CoMutex
{
public:
    CoMutex();
    ~CoMutex();

    void                        Lock();
    void                        UnLock();
    int                         TryLock(int ms);

protected:
    void                        _Lock();
    void                        _UnLock();
private:
    std::queue<CoCond::SPtr>    m_wait_lock_queue;
    std::mutex                  m_mutex;
    CoMutexStatus               m_status{CoMutexStatus::COMUTEX_FREE};
};

} // bbt::coroutine::sync