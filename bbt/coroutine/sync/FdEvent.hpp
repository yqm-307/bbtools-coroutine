#pragma once
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoEventBase.hpp>

namespace bbt::coroutine::sync
{

class FdEvent:
    public bbt::coroutine::detail::CoEventBase,
    public bbt::templateutil::noncopyable
{
public:
    friend class detail::CoPoller;

    static std::shared_ptr<FdEvent> Create();

    BBTATTR_FUNC_Ctor_Hidden
    FdEvent();
    ~FdEvent();

    int                         WaitUntilReadable(int fd, int ms = -1);
    int                         WaitUntilWriteable(int fd, int ms = -1);
private:
    virtual int                 Trigger(short trigger_events, int customkey) override;

    detail::CoPollEventId       m_id{0};
    std::mutex                  m_mutex;
};

} // namespace bbt::coroutine::sync