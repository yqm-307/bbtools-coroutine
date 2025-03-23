#pragma once
#include <boost/noncopyable.hpp>
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::detail
{

class Defer:
    boost::noncopyable
{
public:
    explicit Defer(DeferCallback&& defer_cb);
    Defer(Defer&& other);
    ~Defer();

private:
    DeferCallback m_defer_handler{nullptr};
};

}