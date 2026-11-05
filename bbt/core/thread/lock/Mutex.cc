#include <bbt/core/thread/lock/Mutex.hpp>

namespace bbt::core::thread {

Mutex::Mutex() : m_mutex(PTHREAD_MUTEX_INITIALIZER)
{
    // 初始化列表的 PTHREAD_MUTEX_INITIALIZER 只是占位，真正生效的是下面带递归属性的 init；
    // settype 必须排在 init 之前。
    pthread_mutexattr_init(&m_mutex_attr);
    pthread_mutexattr_settype(&m_mutex_attr, PTHREAD_MUTEX_RECURSIVE);
    // 返回值未检查：init 失败时这把锁留在未初始化状态，后续 lock/unlock 都是未定义行为。
    pthread_mutex_init(&m_mutex, &m_mutex_attr);
}

Mutex::~Mutex()
{
    // 只销毁 mutex，attr 不销毁；destroy 的返回值（如仍有线程持锁时的 EBUSY）也丢弃。
    pthread_mutex_destroy(&m_mutex);
}

void Mutex::lock()
{
    // 返回值丢弃：EINVAL 之类的错误不会上报给调用者。
    pthread_mutex_lock(&m_mutex);
}

void Mutex::unlock()
{
    pthread_mutex_unlock(&m_mutex);
}

bool Mutex::try_lock()
{
    // 非阻塞一次尝试，EBUSY 表示他人持有；递归锁下同线程重入同样返回 true。
    return pthread_mutex_trylock(&m_mutex) == 0;
}

pthread_mutex_t& Mutex::getlock()
{
    // 只借出引用，不转移所有权；调用者应已持有这把锁（pthread_cond_wait 的前置条件）。
    return m_mutex;
}

}
