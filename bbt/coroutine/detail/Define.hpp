#pragma once
#include <functional>
#include <memory>
#include <cstdint>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <bbt/base/Logger/DebugPrint.hpp>


#define g_scheduler bbt::coroutine::detail::Scheduler::GetInstance()

/* 当前线程正在运行的 coroutine */
#define g_bbt_coroutine_co (bbt::coroutine::detail::Processer::GetLocalProcesser()->GetCurrentCoroutine());

#define g_bbt_poller (CoPoller::GetInstance())

namespace bbt::coroutine::sync
{

class Chan;
class CoCond;


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

}

namespace bbt::coroutine::detail
{

class Coroutine;
class Scheduler;
class Processer;
class CoPollEvent;
class CoPoller;

typedef uint64_t CoroutineId;
#define BBT_COROUTINE_INVALID_COROUTINE_ID 0

typedef uint64_t ProcesserId;
#define BBT_COROUTINE_INVALID_PROCESSER_ID 0


typedef std::function<void()> CoroutineCallback;        // 协程处理主函数
typedef std::function<void()> CoroutineFinalCallback;
typedef std::function<void(std::shared_ptr<CoPollEvent>, std::shared_ptr<Coroutine>)> CoPollEventCallback;      // Poller监听事件完成回调

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

}