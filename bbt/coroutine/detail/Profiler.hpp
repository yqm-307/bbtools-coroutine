#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/detail/Processer.hpp>

namespace bbt::coroutine::detail
{

class Profiler
{
public:
    typedef std::unique_ptr<Profiler> UPtr;
    static UPtr& GetInstance();

    Profiler() {}
    ~Profiler() {}

    void                        OnEvent_StartScheudler();

    /* Scheduler 管理的 worker 与其权威集合在同一准备/提交批次登记；
     * 单个惰性 P 的登记保留本入口，分配失败正常传播，不静默漏登记。 */
    void                        OnEvent_StartProcesser(Processer::SPtr proc);
    void                        OnEvent_StopPorcesser(Processer::SPtr proc) noexcept;

    void                        OnEvent_RegistCoroutine();
    void                        OnEvent_DoneCoroutine();

    void                        OnEvent_CreateCoroutine();
    void                        OnEvent_DestoryCoroutine();

    void                        OnEvent_RegistCoPollEvent();
    void                        OnEvent_TriggerCoPollEvent();

    void                        OnEvent_StealSucc(int num);

    void                        OnEvent_StackRelease(int num);
    void                        OnEvent_StackAlloc();

    void                        OnEvent_CoMutexLockYield();

    void                        ProfileInfo(std::string& info);

    // 紧凑格式输出到 stderr，用于压测时每秒采样
    void                        DumpStderr();

    /**
     * @brief 清空统计计数（测试与重复采样的辅助接口）
     */
    void                        Reset();

    struct ProcesserProfile
    {
        ProcesserId id;

    };
private:
    /* 批次提交锁序：Scheduler lifecycle -> Scheduler map -> Profiler map。
     * Profiler 读者只取自身 map 锁，不能反向取 Scheduler 的锁。 */
    friend class Scheduler;
    std::atomic_uint64_t        m_total_regist_co_count{0}; /* 总注册协程数量 */
    std::atomic_uint64_t        m_total_done_co_count{0};   /* 总完成协程数量 */

    std::atomic_int             m_create_coroutine_count{0};
    std::atomic_int             m_destory_coroutine_count{0};

    std::atomic_uint64_t        m_total_steal_count{0};

    bbt::core::clock::Timestamp<>     m_scheduler_begin_timestamp;    /* 调度器开启时间 */



    /* Processer 相关指标 */
    /* ProfileInfo 锁内拷贝强引用、锁外格式化；Scheduler 提交前构造完整副本，
     * 两份 registry 同时封锁读者后无分配 swap，join 后才移除退役记录。 */
    std::map<ProcesserId, Processer::SPtr>
                                m_processer_map;
    std::mutex                  m_processer_map_mutex;

    /* CoEvent 相关指标 */
    std::atomic_uint64_t        m_regist_event_count{0};
    std::atomic_uint64_t        m_trigger_event_count{0};

    /* StackPool 相关指标 */
    std::atomic_uint64_t        m_stack_release_count{0};
    std::atomic_uint64_t        m_stack_alloc_count{0};

    /* CoMutex 相关 */
    std::atomic_uint64_t        m_comutex_lock_count{0};           /* CoMutex发生竞争导致挂起次数 */
};

}