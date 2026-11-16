#include <bbt/core/util/Assert.hpp>
#include <bbt/core/thread/lock/Spinlock.hpp>

namespace bbt::core::thread {

Spinlock::Spinlock()
{
    /**
     * NDEBUG 下 Assert 只打印不终止，init 失败后 m_spin 仍未初始化。
     */
    Assert(pthread_spin_init(&m_spin, 0) == 0);
}

Spinlock::~Spinlock()
{
    /**
     * 仍有线程持有或正在自旋时 destroy 会失败，同样只有 Assert，不中断析构。
     */
    Assert(pthread_spin_destroy(&m_spin) == 0);
}

void Spinlock::lock()
{
    /**
     * 忙等直到拿到锁；同线程重入是未定义行为，glibc 下表现为永久自旋。
     */
    pthread_spin_lock(&m_spin);
}

void Spinlock::unlock()
{
    pthread_spin_unlock(&m_spin);
}

bool Spinlock::try_lock()
{
    /**
     * 单次尝试，不等待。
     */
    return pthread_spin_trylock(&m_spin) == 0;
}

}
