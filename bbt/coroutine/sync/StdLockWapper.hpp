#pragma once
#include <memory>
#include <bbt/coroutine/sync/interface/ICoLock.hpp>

namespace bbt::coroutine::sync
{

class StdLockWapper
{
public:
    StdLockWapper(std::shared_ptr<ICoLock> colock): m_co_lock(colock) {};
    ~StdLockWapper() {}

    void lock() { m_co_lock->Lock(); }
    void unlock() { m_co_lock->UnLock(); }
    bool try_lock() { return (m_co_lock->TryLock() == 0); }

private:
    std::shared_ptr<ICoLock> m_co_lock{nullptr};
};

} // namespace bbt::coroutine::sync