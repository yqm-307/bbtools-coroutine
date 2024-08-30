#include <bbt/coroutine/detail/CoEventBase.hpp>

namespace bbt::coroutine::detail
{

CoPollEventId CoEventBase::_GenerateId()
{
    static std::atomic_uint64_t _id{0};
    return (++_id);
}

CoPollEventId CoEventBase::GetId() const
{
    return m_id;
}

}