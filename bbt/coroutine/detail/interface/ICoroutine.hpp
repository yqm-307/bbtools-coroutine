#pragma once
#include <bbt/coroutine/detail/Stack.hpp>
#include <boost/context/detail/fcontext.hpp>

namespace bbt::coroutine::detail
{

class ICoroutine
{
public:
    virtual void                    Resume() = 0;
    virtual void                    Yield() = 0;
    virtual void                    YieldWithCallback(const CoroutineOnYieldCallback& cb) = 0;

    virtual CoroutineId             GetId() = 0;
};

}