#pragma once
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/pollevent/EventLoop.hpp>
#include <bbt/coroutine/detail/interface/IPollEvent.hpp>


namespace bbt::coroutine::detail
{

class CoEventBase:
    public IPollEvent
{
public:
    CoEventBase();
    ~CoEventBase();

    virtual CoPollEventId               GetId() const override final;
    virtual std::shared_ptr<Coroutine>  GetWaitCo() const override;
private:
    static CoPollEventId                _GenerateId();

    virtual int                         InitFdEvent(int fd, short event, int ms) override;
    virtual void                        InitCustomEvent(CoPollEventCustom custom_key) override;
    virtual int                         Regist() override;

    const CoPollEventId m_id{0};
    std::shared_ptr<Coroutine> m_wait_coroutine{nullptr};
    std::shared_ptr<pollevent::Event>   m_event;
    CoPollEventCustom                   m_custom_key{POLL_EVENT_CUSTOM_DEFAULT};
    int                                 m_timeout{-1};
};

}