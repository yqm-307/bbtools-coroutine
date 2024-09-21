#pragma once

namespace bbt::coroutine::sync
{

class ICoLock
{
public:
    virtual void Lock() = 0;
    virtual void UnLock() = 0;
    virtual int TryLock() = 0;
    virtual int TryLock(int ms) = 0;

};

} // namespace bbt::coroutine::sync
