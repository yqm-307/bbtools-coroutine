#include <bbt/core/thread/lock/Mutex.hpp>

namespace bbt::core::thread
{

Mutex::Mutex():
    m_mutex(PTHREAD_MUTEX_INITIALIZER)
{
    pthread_mutexattr_init(&m_mutex_attr);
    pthread_mutexattr_settype(&m_mutex_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m_mutex, &m_mutex_attr);
}

Mutex::~Mutex()
{
    pthread_mutex_destroy(&m_mutex);
}

void Mutex::lock()
{
    pthread_mutex_lock(&m_mutex);
}

void Mutex::unlock()
{
    pthread_mutex_unlock(&m_mutex);
}

bool Mutex::try_lock()
{
    return pthread_mutex_trylock(&m_mutex) == 0;
}

pthread_mutex_t& Mutex::getlock()
{
    return m_mutex;
}

}