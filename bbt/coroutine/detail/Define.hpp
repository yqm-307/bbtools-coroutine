#include <functional>
#include <cstdint>

namespace bbt::coroutine::detail
{

typedef uint64_t CoroutineId;
#define BBT_COROUTINE_INVALID_COROUTINE_ID 0

typedef std::function<void()> CoroutineCallback;        // 协程处理主函数
typedef std::function<void()> CoroutineFinalCallback;

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
    Default = 0,    // 默认，尚未初始化
    Pending = 1,    // 等待中，尚未开始
    Running = 2,    // 运行中
    Suspend = 3,    // 挂起
    Final   = 4,    // 执行结束
};

class Coroutine;
class Scheduler;
class Processer;
}