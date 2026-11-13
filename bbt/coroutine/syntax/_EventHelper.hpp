/**
 * @file _EventHelper.hpp
 * @brief bbtco_ev_* 宏的事件帮手：一次性 fd/定时器事件的注册器。
 */
#pragma once
#include <bbt/coroutine/detail/CoPoller.hpp>

namespace bbt::coroutine {

/**
 * @brief 把「fd/定时器就绪后跑一段回调」注册成一次性事件，bbtco_ev_* 宏的实现；宏里的临时 shared_ptr 表达式
 *        结束即销毁，靠捕获进 Event 回调的 shared_from_this() 活到触发，触发时回调置空 m_pollevent 断环。
 */
class _EventHelper : public std::enable_shared_from_this<_EventHelper> {
public:
    /**
     * @brief 登记 fd/定时器事件；纯定时器 fd 传 -1，ms 为超时毫秒，纯定时器须给有效期限。
     * @param event 事件位（READABLE/WRITEABLE/TIMEOUT/FINALIZE），必须非空且不含 PERSIST。
     */
    explicit _EventHelper(int fd, short event, int ms);

    /**
     * @brief 同上，但触发后的回调交给指定 CoPool 执行；池只以 weak_ptr 持有，池已销毁则回退到直接注册协程。
     */
    explicit _EventHelper(int fd, short event, int ms, std::shared_ptr<pool::CoPool> pool);

    /**
     * @brief 释放自身对 Event 的持有（析构体为空）；外部释放 owner 不是取消入口，等待中的事件不会因此取消。
     */
    ~_EventHelper();

    /**
     * @brief 创建 Event 并开始监听；触发后在 poller 线程投递、以新协程（或池任务）执行 event_handle（可挂起）。
     * @return -1 参数检查失败（事件位为空）或 StartListen 失败，此时回调不会被调用；0 已登记。
     */
    int operator-(const ExtCoEventCallback& event_handle);

private:
    /**
     * 持有到触发为止；回调里置空是为了断开 helper → Event → 回调 → helper 的环
     */
    std::shared_ptr<bbt::pollevent::Event> m_pollevent{nullptr};

    /**
     * 只弱引用：池的寿命由创建者决定，事件登记不延长它
     */
    std::weak_ptr<pool::CoPool> m_copool;

    int m_fd{-1};
    int m_timeout{-1};

    short m_event{-1};

    /**
     * 构造时的事件位校验结果；为 false 时 operator- 直接返回 -1，不去碰 Event
     */
    bool m_arg_check_rlt{false};
};

}
