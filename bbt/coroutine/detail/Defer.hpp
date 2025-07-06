#pragma once
#include <boost/noncopyable.hpp>
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::detail
{

class Defer final:
    boost::noncopyable
{
public:
    explicit Defer(const DeferCallback& cb);
    ~Defer();

private:
    DeferCallback m_defer_handler{nullptr};
};

}