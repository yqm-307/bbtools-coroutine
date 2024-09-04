#pragma once
#include <bbt/base/templateutil/Noncopyable.hpp>
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::detail
{

class Defer:
    bbt::templateutil::noncopyable
{
public:
    explicit Defer(DeferCallback&& defer_cb);
    Defer(Defer&& other);
    ~Defer();

private:
    DeferCallback m_defer_handler{nullptr};
};

}