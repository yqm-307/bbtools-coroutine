
#include <bbt/core/util/Assert.hpp>
#include <bbt/core/thread/lock/Spinlock.hpp>

namespace bbt::core::thread
{

Spinlock::Spinlock()
{
    Assert(pthread_spin_init(&m_spin, 0) == 0);
}

Spinlock::~Spinlock()
{
    Assert(pthread_spin_destroy(&m_spin) == 0);
}

void Spinlock::lock()
{
    pthread_spin_lock(&m_spin);
}

void Spinlock::unlock()
{
    pthread_spin_unlock(&m_spin);
}

bool Spinlock::try_lock()
{
    return pthread_spin_trylock(&m_spin) == 0;
}

}