/**
 * @file Profiler.hpp
 * @brief 运行时计数器与 worker 快照：ProfileInfo 输出可读文本，DumpStderr 供压测逐秒采样。
 */
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/detail/Processer.hpp>

namespace bbt::coroutine::detail {

class Profiler {
public:
    typedef std::unique_ptr<Profiler> UPtr;
    static UPtr& GetInstance();

    Profiler()
    {
    }
    ~Profiler()
    {
    }

    void OnEvent_StartScheudler();

    /* Scheduler 管理的 worker 与其权威集合在同一准备/提交批次登记；
     * 单个惰性 P 的登记保留本入口，分配失败正常传播，不静默漏登记。 */
    void OnEvent_StartProcesser(Processer::SPtr proc);
    void OnEvent_StopPorcesser(Processer::SPtr proc) noexcept;

    /* 以下 OnEvent_* 由热路径直接调用：只做一次原子自增（OnEvent_StartProcesser 例外，要登记
     * worker 强引用），不加锁、不分配、不挂起。 */
    void OnEvent_RegistCoroutine();
    void OnEvent_DoneCoroutine();

    void OnEvent_CreateCoroutine();
    void OnEvent_DestoryCoroutine();

    void OnEvent_RegistCoPollEvent();
    void OnEvent_TriggerCoPollEvent();

    void OnEvent_StealSucc(int num);

    void OnEvent_StackRelease(int num);
    void OnEvent_StackAlloc();

    void OnEvent_CoMutexLockYield();

    /** @brief 生成多行可读统计文本：计数、StackPool 指标与每个 worker 的实时数字。
     *  worker 列表锁内只拷强引用，格式化在锁外做；退役 worker 由 reaper 在 join 后移除。 */
    void ProfileInfo(std::string& info);

    /* 紧凑格式输出到 stderr，用于压测时每秒采样。与 ProfileInfo 不同：整段持
     * m_processer_map_mutex 直接写 stderr，采样线程阻塞期间会挡住 worker 登记/退役。 */
    void DumpStderr();

    /**
     * @brief 清空统计计数（测试与重复采样的辅助接口）
     */
    void Reset();

    struct ProcesserProfile {
        ProcesserId id;
    };

private:
    /* 批次提交锁序：Scheduler lifecycle -> Scheduler map -> Profiler map。
     * Profiler 读者只取自身 map 锁，不能反向取 Scheduler 的锁。 */
    friend class Scheduler;
    std::atomic_uint64_t m_total_regist_co_count{0}; /* 总注册协程数量 */
    std::atomic_uint64_t m_total_done_co_count{0};   /* 总完成协程数量 */

    std::atomic_int m_create_coroutine_count{0};
    std::atomic_int m_destory_coroutine_count{0};

    std::atomic_uint64_t m_total_steal_count{0};

    bbt::core::clock::Timestamp<> m_scheduler_begin_timestamp; /* 调度器开启时间 */

    /* Processer 相关指标 */
    /* ProfileInfo 锁内拷贝强引用、锁外格式化；Scheduler 提交前构造完整副本，
     * 两份 registry 同时封锁读者后无分配 swap，join 后才移除退役记录。 */
    std::map<ProcesserId, Processer::SPtr> m_processer_map;
    std::mutex m_processer_map_mutex;

    /* CoEvent 相关指标 */
    std::atomic_uint64_t m_regist_event_count{0};
    std::atomic_uint64_t m_trigger_event_count{0};

    /* StackPool 相关指标 */
    std::atomic_uint64_t m_stack_release_count{0};
    std::atomic_uint64_t m_stack_alloc_count{0};

    /* CoMutex 相关 */
    std::atomic_uint64_t m_comutex_lock_count{0}; /* CoMutex发生竞争导致挂起次数 */
};

}