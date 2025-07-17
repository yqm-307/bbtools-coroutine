#pragma once
#include <bbt/coroutine/detail/CoPoller.hpp>

namespace bbt::coroutine
{

/**
 * 为了实现协程的事件驱动，提供了一个事件帮助类。
 * 
 * 可以非常有效的帮助开发者通过事件来控制协程挂起和唤醒。
 */
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
    std::weak_ptr<pool::CoPool> m_copool;

    int m_fd{-1};
    int m_timeout{-1};

    short m_event{-1};
    bool m_arg_check_rlt{false};
};

} // bbt::coroutine