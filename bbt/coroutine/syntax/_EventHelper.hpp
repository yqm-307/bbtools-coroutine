#pragma once
#include <bbt/coroutine/detail/CoPoller.hpp>

namespace bbt::coroutine
{

class _EventHelper:
    public std::enable_shared_from_this<_EventHelper>
{
public:
    explicit _EventHelper(int fd, short event, int ms);
    ~_EventHelper();

    int operator-(const std::function<void()>& event_handle);

private:
    std::shared_ptr<bbt::pollevent::Event> m_pollevent{nullptr};
    int m_fd{-1};
    short m_event{-1};
    int m_timeout{-1};
    bool m_arg_check_rlt{false};
};

} // bbt::coroutine