#pragma once
#include <bbt/coroutine/detail/CoPoller.hpp>

namespace bbt::coroutine
{

class _EventHelper:
    public std::enable_shared_from_this<_EventHelper>
{
public:
    explicit _EventHelper(int fd, short event, int ms);
    explicit _EventHelper(int fd, short event, int ms, std::shared_ptr<pool::CoPool> pool);
    ~_EventHelper();

    int operator-(const ExtCoEventCallback& event_handle);

private:
    std::shared_ptr<bbt::pollevent::Event> m_pollevent{nullptr};
    int m_fd{-1};
    short m_event{-1};
    int m_timeout{-1};
    std::weak_ptr<pool::CoPool> m_copool;
    bool m_arg_check_rlt{false};
};

} // bbt::coroutine