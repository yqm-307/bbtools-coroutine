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
    friend class detail::CoPoller;

    CoEventBase();
    ~CoEventBase();

    virtual CoPollEventId               GetId() const override final;
    virtual std::shared_ptr<Coroutine>  GetWaitCo() const override;
protected:
    virtual int                         OnNotify(short tirgger_events, int custom_key) = 0;
    virtual int                         InitFdEvent(int fd, short event, int ms) override;
    virtual void                        InitCustomEvent(CoPollEventCustom custom_key) override;
    virtual int                         Regist() override;
private:
    virtual int                         Trigger(short trigger_events, int customkey) final override;
    static CoPollEventId                _GenerateId();


    const CoPollEventId m_id{0};
    std::shared_ptr<Coroutine>          m_wait_coroutine{nullptr};
    std::shared_ptr<pollevent::Event>   m_event{nullptr};
    CoPollEventCustom                   m_custom_key{POLL_EVENT_CUSTOM_DEFAULT};
    int                                 m_timeout{-1};
};

}