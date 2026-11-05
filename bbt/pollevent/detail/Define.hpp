/**
 * @file bbt/pollevent/detail/Define.hpp
 * @brief pollevent 的公共词汇：事件类型位、循环模式、线程状态、回调签名与兼容宏；文件底部 EV_* 宏与本文件枚举同值，须同步改。
 */
#pragma once
#include <memory>
#include <functional>
#include <sys/time.h>
#include <bbt/core/util/Assert.hpp>
#include <bbt/core/errcode/Errcode.hpp>

namespace bbt::pollevent {

namespace detail {
class EventBase;
} // namespace detail

class Event;
class EventLoop;

/**
 * @brief 事件类型标志位，数值与 libevent 一致，可与 EV_* 宏互换。
 * READABLE/WRITEABLE 建 fd 等待，TIMEOUT 建定时器，PERSIST 交付后自动重挂；SIGNAL/FINALIZE 只占位，CLOSE 只参与 fd 事件判定。
 */
enum EventOpt : short {
    TIMEOUT = 0x01,
    READABLE = 0x02,
    WRITEABLE = 0x04,
    SIGNAL = 0x08,
    PERSIST = 0x10,
    CLOSE = 0x80,
    FINALIZE = 0x40,
};

/* ── 事件循环选项 ──
 *
 * LOOP_NORMAL:           阻塞，处置一个就绪事件后返回
 * LOOP_ONCE:             阻塞，等待一个事件就绪并处置后返回
 * LOOP_NONBLOCK:         非阻塞，仅处置已就绪事件，无就绪立即返回
 * LOOP_NO_EXIT_ON_EMPTY: 阻塞，无就绪事件时不退出，继续等待
 */

/** @brief 事件循环的运行模式；NO_EXIT_ON_EMPTY 当前仅 restart 后 run_one，空 context 仍会返回。 */
enum EventLoopOpt {
    LOOP_NORMAL = 0,
    LOOP_ONCE = 0x01,
    LOOP_NONBLOCK = 0x02,
    LOOP_NO_EXIT_ON_EMPTY = 0x04,
};

/**
 * @brief EvThread 的生命周期：Default → Running → Finish；由 Work() 写，其它线程只读。
 * m_status 是 volatile 而非原子量，跨线程读取不构成同步，不能当作线程已启动/已退出的证明。
 */
enum EvThreadRunStatus {
    Default = 0,
    Running = 1,
    Finish = 2,
};

/** @brief pollevent 自己的错误码取值，供 EvThread::Stop 的失败返回使用。 */
enum emErr : bbt::core::errcode::ErrType {
    ERR_UNKNOWN = 0,
};

/* ── 类型别名 ── */
/** @brief 进程内自增的事件标识；回调注册表以它为 key，Event 析构后不复用。 */
typedef int64_t EventId;
/** @brief 进程内自增的 IO 线程 id；小于 0 表示未分配。 */
typedef int EvThreadId;
#ifndef evutil_socket_t
/** @brief 兼容 libevent 的 fd 类型；未引入 libevent 时退化为 int。 */
typedef int evutil_socket_t; // 替代 libevent 的 evutil_socket_t
#endif
/**
 * @brief 事件交付回调：fd 为用户 fd（定时器为 -1），events 为本次交付的标志位，id 为 EventId。
 * 在驱动 EventLoop 的线程内调用，可重入 StartListen/CancelListen；在其中长时间阻塞会停掉整条派发。
 */
typedef std::function<void(int fd, short events, EventId id)> OnEventCallback;

/* ── 初始化（不再需要 evthread_use_pthreads）── */
/* 兼容占位：构造无实际动作，保留静态实例只为旧引用能编译。 */
struct __PollEvent_Init {
    __PollEvent_Init()
    {
    }
};

static __PollEvent_Init __detail_init_helper;

} // namespace bbt::pollevent

/* 反向兼容：让 EvThread.cc 中的 EVLOOP_NO_EXIT_ON_EMPTY 保持可用 */
#ifndef EV_READ
/** @brief 可读事件位，同 EventOpt::READABLE。 */
#define EV_READ     0x02
/** @brief 可写事件位，同 EventOpt::WRITEABLE。 */
#define EV_WRITE    0x04
/** @brief 超时事件位，同 EventOpt::TIMEOUT。 */
#define EV_TIMEOUT  0x01
/** @brief 信号事件位，同 EventOpt::SIGNAL，当前只占位。 */
#define EV_SIGNAL   0x08
/** @brief 持久事件位，同 EventOpt::PERSIST；当前仅 fd 就绪后重挂，不使定时器周期化。 */
#define EV_PERSIST  0x10
/** @brief 对端关闭事件位，同 EventOpt::CLOSE。 */
#define EV_CLOSED   0x80
/** @brief 终结事件位，同 EventOpt::FINALIZE，当前只占位。 */
#define EV_FINALIZE 0x40
#endif

#ifndef EVLOOP_NO_EXIT_ON_EMPTY
/** @brief 循环选项，同 EventLoopOpt::LOOP_ONCE。 */
#define EVLOOP_ONCE             0x01
/** @brief 循环选项，同 EventLoopOpt::LOOP_NONBLOCK。 */
#define EVLOOP_NONBLOCK         0x02
/** @brief 循环选项，同 EventLoopOpt::LOOP_NO_EXIT_ON_EMPTY。 */
#define EVLOOP_NO_EXIT_ON_EMPTY 0x04
#endif
