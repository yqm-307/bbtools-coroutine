#pragma once
#include <bbt/coroutine/detail/Stack.hpp>
#include <boost/context/detail/fcontext.hpp>

namespace bbt::coroutine::detail
{

/**
 * 稳定 C++ 表面：Create 在 Coroutine 上；对象上可 Resume / Yield /
 * GetId / GetStatus。Cancel / GetException 见 #266 / #267。
 */
class ICoroutine
{
public:
    virtual void                    Resume() = 0;
    virtual void                    Yield() = 0;
    virtual int                     YieldWithCallback(const CoroutineOnYieldCallback& cb) = 0;

    virtual CoroutineId             GetId() const noexcept = 0;
    virtual CoroutineStatus         GetStatus() const noexcept = 0;
};

}
