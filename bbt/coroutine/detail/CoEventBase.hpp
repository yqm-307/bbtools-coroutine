#pragma once
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/interface/IPollEvent.hpp>


namespace bbt::coroutine::detail
{

class CoEventBase:
    public IPollEvent
{
public:
    CoEventBase():m_id(_GenerateId()) {}
    ~CoEventBase() {}

    virtual CoPollEventId GetId() const override final;
private:
    static CoPollEventId _GenerateId();

    const CoPollEventId m_id;
};

}