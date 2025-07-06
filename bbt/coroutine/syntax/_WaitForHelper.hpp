#pragma once
#include <bbt/coroutine/detail/Coroutine.hpp>

namespace bbt::coroutine
{

class _WaitForHelper final:
    public std::enable_shared_from_this<_WaitForHelper>
{
public:
    explicit _WaitForHelper(int fd, short event, int ms);
    virtual ~_WaitForHelper() = default;
};

};