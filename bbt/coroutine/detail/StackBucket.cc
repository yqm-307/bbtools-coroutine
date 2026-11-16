#include <bbt/coroutine/detail/StackBucket.hpp>
#include <bbt/coroutine/detail/StackPool.hpp>

namespace bbt::coroutine::detail {

StackBucket::StackBucket(size_t capacity)
{
    pool.reserve(capacity);
}

StackBucket::~StackBucket()
{
    for (auto* item : pool)
        StackPool::_FreeItem(item);
}

bool StackBucket::Push(Stack* item) noexcept
{
    try {
        pool.push_back(item);
        return true;
    } catch (...) {

        /**
         * 沿用归还失败分支：扣 live 后锁外释放栈。
         */
        return false;
    }
}

bool StackBucket::Pop(Stack*& item) noexcept
{
    if (pool.empty())
        return false;
    item = pool.back();
    pool.pop_back();
    return true;
}

}
