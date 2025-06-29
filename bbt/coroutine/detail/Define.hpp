#pragma once
#include <set>
#include <map>
#include <queue>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <cstdint>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <bbt/core/log/DebugPrint.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/core/Attribute.hpp>
#include <bbt/core/util/Assert.hpp>
#include <bbt/coroutine/utils/lockfree/blockingconcurrentqueue.h>

// #define BBT_COROUTINE_PROFILE

#define g_scheduler                 (bbt::coroutine::detail::Scheduler::GetInstance())

#define g_bbt_tls_helper            (bbt::coroutine::detail::LocalThread::GetTLSInst())

#define g_bbt_tls_processer         (g_bbt_tls_helper->GetProcesser())

/* 当前线程正在运行的 coroutine */
#define g_bbt_tls_coroutine_co      (g_bbt_tls_processer ? g_bbt_tls_processer->GetCurrentCoroutine() : nullptr)

#define g_bbt_poller                (bbt::coroutine::detail::CoPoller::GetInstance())

#define g_bbt_coroutine_config      (bbt::coroutine::detail::GlobalConfig::GetInstance())

#define g_bbt_profiler              (bbt::coroutine::detail::Profiler::GetInstance())

#define g_bbt_stackpoll             (bbt::coroutine::detail::StackPool::GetInstance())

#define g_bbt_dbgmgr                (bbt::coroutine::detail::DebugMgr::GetInstance())

namespace bbt::coroutine
{


enum SchedulerStartOpt
{
    SCHE_START_OPT_SCHE_THREAD      = 1,    // 线程模式，几乎不阻塞，开启后台调度线程
    SCHE_START_OPT_SCHE_LOOP        = 2,    // 循环模式，阻塞并在当前线程调度
    SCHE_START_OPT_SCHE_NO_LOOP     = 3,    // 非循环模式，需要用户手动LoopOnce来驱动调度
};

/**
 * @brief 对外注册协程事件函数
 * @param fd 套接字，如果不存在则为-1
 * @param event 触发事件
 * 
 * @return 对于persist事件，返回false会自动注销掉当前事件
 */
typedef std::function<void(int /*fd*/, short /*trigger event*/)> ExtCoEventCallback;

namespace pool
{
    
class Work;
class CoPool;

typedef std::function<void()> CoPoolWorkCallback;

}

namespace sync
{

template<class TItem, int Max> class Chan;
class CoWaiter;

class CoCond;
class CoMutex;



/*
@startuml
[*] --> CHAN_DEFAUTL
CHAN_DEFAUTL --> CHAN_OPEN  : 初始化
CHAN_OPEN    --> CHAN_CLOSE : 主动关闭此信道
CHAN_CLOSE   --> [*]
@enduml
*/
enum ChanStatus : int32_t
{
    CHAN_DEFAUTL        = 0,
    CHAN_OPEN           = 1,
    CHAN_CLOSE          = 2,
};

/*
@startuml
[*] --> COND_DEFAULT
COND_DEFAULT    --> COND_FREE
COND_FREE       --> COND_WAIT
COND_WAIT       --> COND_ACTIVE
COND_ACTIVE     --> COND_FREE
@enduml
*/
enum CoCondStatus : int32_t
{
    COND_DEFAULT        = 0,
    COND_FREE           = 1,
    COND_WAIT           = 2,
    COND_ACTIVE         = 3,
};

enum CoMutexStatus
{
    COMUTEX_LOCKED      = 0,
    COMUTEX_FREE        = 1,
};

enum CoRWMutexStatus
{
    CORWMUTEX_RLOCKED   = 0,
    CORWMUTEX_WLOCKED   = 1,
    CORWMUTEX_FREE      = 2,
};

}

namespace detail
{

/**
@startuml
[*] --> Default
Default --> Pending : 初始化协程，调度器将协程分配到global队列或processer队列中
Pending --> Running : processer取出协程执行
Running --> Suspend : 通过Yield主动挂起
Suspend --> Running : processer再次取出执行
Suspend --> Final   : kill掉此协程
Running --> Final   : 协程执行完毕
Final   --> [*]

Pending: **等待任务开始**

Running: **执行任务**

Suspend: **挂起中**

Final: **任务完成**
@enduml
 */
enum CoroutineStatus : int32_t
{
    CO_DEFAULT = 0,    // 默认，尚未初始化
    CO_PENDING = 1,    // 等待中，尚未开始
    CO_RUNNING = 2,    // 运行中
    CO_SUSPEND = 3,    // 挂起
    CO_FINAL   = 4,    // 执行结束
};


/**
@startuml
[*] --> Default
Default --> Suspend : 初始化完成，挂起
Suspend --> Running : 被调度器唤醒
Running --> Suspend : 本地队列执行完

Running: **执行任务**
Suspend: **挂起中**

@enduml
 */
enum ProcesserStatus : int32_t
{
    PROC_DEFAULT = 0,    // 默认。尚未初始化
    PROC_SUSPEND = 1,    // 挂起中
    PROC_RUNNING = 2,    // 运行中。在执行任务
    PROC_EXIT    = 3,    // 运行结束
};

enum ScheudlerStatus: int32_t
{
    SCHE_DEFAULT = 0,   // 默认
    SCHE_RUNNING = 1,   // 执行中
    SCHE_SUSPEND = 2,   // 挂起
    SCHE_EXIT    = 3,   // 结束
};

/*
@startuml
[*] --> POLLEVENT_DEFAULT
POLLEVENT_DEFAULT --> POLLEVENT_INITED : 创建监听事件
POLLEVENT_INITED  --> POLLEVENT_LISTEN : 注册到poller中
POLLEVENT_LISTEN  --> POLLEVENT_TRIGGER: 触发监听事件
POLLEVENT_TRIGGER --> POLLEVENT_FINAL  : 监听完成
POLLEVENT_LISTEN  --> POLLEVENT_CANNEL : 取消监听
POLLEVENT_FINAL   --> [*]
@enduml
 */
enum CoPollEventStatus : int32_t
{
    POLLEVENT_DEFAULT = 0, // 默认
    POLLEVENT_INITED  = 1, // 初始化完成
    POLLEVENT_LISTEN  = 2, // 监听中
    POLLEVENT_TRIGGER = 3, // 触发中
    POLLEVENT_FINAL   = 4, // 监听结束
    POLLEVENT_CANNEL  = 5, // 取消事件
};

/* PollEventType表示触发的事件类型 */
enum PollEventType
{
    POLL_EVENT_DEFAULT      = 0,
    POLL_EVENT_TIMEOUT      = 1 << 0,
    POLL_EVENT_WRITEABLE    = 1 << 1,
    POLL_EVENT_READABLE     = 1 << 2,
    POLL_EVENT_CUSTOM       = 1 << 3,
};

/* CoPollEvent的自定义事件key，用来表示触发时是那个自定义事件 */
enum CoPollEventCustom
{
    POLL_EVENT_CUSTOM_COND  = 1,    // co cond
    POLL_EVENT_CUSTOM_CHAN  = 2,    // chan
    POLL_EVENT_CUSTOM_COMUTEX = 3,  // co mutex
};

enum CoroutinePriority : int32_t
{
    CO_PRIORITY_LOW       = 0,  // 低优先级
    CO_PRIORITY_NORMAL    = 1,  // 普通优先级
    CO_PRIORITY_HIGH      = 2,  // 高优先级
    CO_PRIORITY_CRITICAL  = 3,  // 关键优先级
    CO_PRIORITY_COUNT     = 4,  // 优先级数量
};

class Coroutine;    // 协程
class Scheduler;    // 调度器
class Processer;    // 执行器
class CoPollEvent;  // 协程事件
class CoPoller;     // 事件轮询器
class GlobalConfig; // 全局配置
class LocalThread;  // 线程局部数据辅助类
class DebugMgr;     // dbg管理器
class Defer;        // Defer helper


typedef uint64_t CoroutineId;
#define BBT_COROUTINE_INVALID_COROUTINE_ID 0

typedef uint64_t ProcesserId;
#define BBT_COROUTINE_INVALID_PROCESSER_ID 0

typedef uint64_t CoPollEventId;
#define BBT_COROUTINE_INVALID_COPOLLEVENT_ID 0

typedef moodycamel::BlockingConcurrentQueue<Coroutine*> CoroutineQueue; // 协程队列
typedef std::array<CoroutineQueue, CoroutinePriority::CO_PRIORITY_COUNT> CoPriorityQueue; // 协程优先级队列

typedef std::function<void()> CoroutineCallback;        // 协程处理主函数
typedef std::function<void()> CoroutineFinalCallback;   // 协程主函数执行完毕回调
typedef std::function<bool()> CoroutineOnYieldCallback; // 协程挂起后执行回调
typedef std::function<void()> DeferCallback;            // defer 执行函数

/**
 * @param 触发的事件
 * @param 事件类型
 * @param 仅当事件类型为POLL_EVENT_CUSTOM时，表示枚举类型CoPollEventCustom中的值
 */
typedef std::function<void(std::shared_ptr<CoPollEvent>, int /*events*/, int)> CoPollEventCallback;      // Poller监听事件完成回调

#undef MOODYCAMEL_CPP11_THREAD_LOCAL_SUPPORTED

} // namespace bbt::coroutine::detail

} // namespace bbt::coroutine