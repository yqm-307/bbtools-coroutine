#pragma once
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::pool
{

class Work
{
public:
    Work(CoPoolWorkCallback&& workfunc):m_callback(std::move(workfunc)) {}
    Work(const CoPoolWorkCallback& workfunc):m_callback(workfunc) {}
    virtual ~Work() {}
    virtual void Invoke()
    {
        m_callback();
    }
private:
    CoPoolWorkCallback m_callback{nullptr};
};

}