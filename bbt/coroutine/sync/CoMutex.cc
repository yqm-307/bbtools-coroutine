#include <bbt/coroutine/sync/CoMutex.hpp>

namespace bbt::coroutine::sync
{

CoMutex::CoMutex()
{

}

CoMutex::~CoMutex()
{

}

void CoMutex::Lock()
{
    _Lock();

    /* 如果已经上锁了，挂起并加入到等待队列中 */
    if (m_status == CoMutexStatus::COMUTEX_LOCKED) {
        
    }

    m_status = CoMutexStatus::COMUTEX_LOCKED;

    _UnLock();
}

void CoMutex::UnLock()
{

}

int CoMutex::TryLock(int ms)
{

}

void CoMutex::_Lock()
{
    m_mutex.lock();
}

void CoMutex::_UnLock()
{
    m_mutex.unlock();
}

}