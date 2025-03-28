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

    void                        OnEvent_StartProcesser(Processer::SPtr proc);
    void                        OnEvent_StopPorcesser(Processer::SPtr proc);

    void                        OnEvent_RegistCoroutine();
    void                        OnEvent_DoneCoroutine();

    void                        OnEvent_CreateCoroutine();
    void                        OnEvent_DestoryCoroutine();

    void                        OnEvent_RegistCoPollEvent();
    void                        OnEvent_TriggerCoPollEvent();

    void                        OnEvent_StealSucc(int num);

    void                        OnEvent_StackRelease(int num);
    void                        OnEvent_StackAlloc();

    void                        ProfileInfo(std::string& info);

    struct ProcesserProfile
    {
        ProcesserId id;
        
    };
private:
    std::atomic_uint64_t        m_total_regist_co_count{0}; /* 总注册协程数量 */
    std::atomic_uint64_t        m_total_done_co_count{0};   /* 总完成协程数量 */

    std::atomic_int             m_create_coroutine_count{0};
    std::atomic_int             m_destory_coroutine_count{0};

    std::atomic_uint64_t        m_total_steal_count{0};             

    bbt::core::clock::Timestamp<>     m_scheduler_begin_timestamp;    /* 调度器开启时间 */



    /* Processer 相关指标 */
    std::map<ProcesserId, Processer::SPtr>
                                m_processer_map;
    std::mutex                  m_processer_map_mutex;

    /* CoEvent 相关指标 */
    std::atomic_uint64_t        m_regist_event_count{0};
    std::atomic_uint64_t        m_trigger_event_count{0};

    /* StackPool 相关指标 */
    std::atomic_uint64_t        m_stack_release_count{0};
    std::atomic_uint64_t        m_stack_alloc_count{0};
};

}