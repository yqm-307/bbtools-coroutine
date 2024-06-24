#include <atomic>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>

namespace bbt::coroutine::detail
{

Processer::SPtr Processer::Create()
{
    return std::make_shared<Processer>();
}

Processer::SPtr Processer::GetLocalProcesser()
{
    static thread_local Processer::SPtr tl_processer = nullptr;
    if (tl_processer == nullptr) {
        tl_processer = Create();
#ifdef BBT_COROUTINE_PROFILE
        g_bbt_profiler->OnEvent_StartProcesser(tl_processer);
#endif
    }

    return tl_processer;
}


ProcesserId Processer::_GenProcesserId()
{
    static std::atomic_uint64_t _generate_id{BBT_COROUTINE_INVALID_PROCESSER_ID};
    return (++_generate_id);
}

Processer::Processer():
    m_id(_GenProcesserId())
{
    m_run_status = ProcesserStatus::PROC_SUSPEND;
}

Processer::~Processer()
{
}

ProcesserStatus Processer::GetStatus()
{
    return m_run_status;
}

int Processer::GetLoadValue()
{
    Assert(m_coroutine_queue.Size() >= 0);
    return m_coroutine_queue.Size();
}

int Processer::GetExecutableNum()
{
    return (m_coroutine_queue.Size() + m_actived_queue.Size());
}


ProcesserId Processer::GetId()
{
    return m_id;
}

void Processer::AddCoroutineTask(Coroutine::SPtr coroutine)
{
    m_coroutine_queue.PushTail(coroutine);
    coroutine->BindProcesser(shared_from_this());
    _OnAddCorotinue();
}

void Processer::AddCoroutineTaskRange(std::vector<Coroutine::SPtr>::iterator begin, std::vector<Coroutine::SPtr>::iterator end)
{
    m_coroutine_queue.PushTailRange(begin, end);
    for (auto it = begin; it != end; ++it)
        (*it)->BindProcesser(shared_from_this());
    _OnAddCorotinue();
}

void Processer::Start(bool background_thread)
{
    if (background_thread)
    {
        auto t = new std::thread([this](){_Run();});
        t->detach();
        return;
    }

    _Run();
}

void Processer::_Run()
{
    while (m_is_running)
    {
        m_run_status = ProcesserStatus::PROC_RUNNING;

        while (_TryGetCoroutineFromGlobal() > 0)
        {
            std::vector<Coroutine::SPtr> actived_coroutines;
            std::vector<Coroutine::SPtr> pending_coroutines;
            m_actived_queue.PopAll(actived_coroutines);
            m_coroutine_queue.PopAll(pending_coroutines);
            /* 优先被激活的挂起协程 */
            for (auto&& coroutine : actived_coroutines) {
                m_running_coroutine = coroutine;
                AssertWithInfo(m_running_coroutine != nullptr, "maybe coroutine queue has bug!");
                m_co_swap_times++;
                m_running_coroutine->Resume();
            }

            for (auto&& coroutine : pending_coroutines) {
                m_running_coroutine = coroutine;
                AssertWithInfo(m_running_coroutine != nullptr, "maybe coroutine queue has bug!");
                m_co_swap_times++;
                m_running_coroutine->Resume();
            }
        }

        std::unique_lock<std::mutex> lock_uptr(m_run_cond_mutex);
        m_run_status = ProcesserStatus::PROC_SUSPEND;
        m_run_cond.wait_for(lock_uptr, bbt::clock::milliseconds(1));
    }

    m_run_status = ProcesserStatus::PROC_EXIT;
}

void Processer::Stop()
{
    do {
        m_is_running = false;
        std::this_thread::sleep_for(bbt::clock::milliseconds(50));
        m_run_cond.notify_one();
    } while (m_run_status != ProcesserStatus::PROC_EXIT);
}

void Processer::_OnAddCorotinue()
{
    // if (!m_is_running || !(m_run_status == ProcesserStatus::PROC_Suspend))
    if (!m_is_running)
        return;

    m_run_cond.notify_one();
}

size_t Processer::_TryGetCoroutineFromGlobal()
{
    std::vector<Coroutine::SPtr> vec;
    g_scheduler->GetGlobalCoroutine(vec, g_bbt_coroutine_config->m_cfg_processer_get_co_from_g_count);
    for (auto&& co : vec)
        co->BindProcesser(shared_from_this());
    m_coroutine_queue.PushTailRange(vec.begin(), vec.end());
    return vec.size();
}


void Processer::Notify()
{
    m_run_cond.notify_one();
}

Coroutine::SPtr Processer::GetCurrentCoroutine()
{
    return m_running_coroutine;
}

void Processer::AddActiveCoroutine(Coroutine::SPtr actived_coroutine)
{
    m_actived_queue.PushTail(actived_coroutine);
}

void Processer::AddActiveCoroutine(std::vector<Coroutine::SPtr> coroutines)
{
    m_actived_queue.PushTailRange(coroutines.begin(), coroutines.end());
}

uint64_t Processer::GetContextSwapTimes()
{
    return m_co_swap_times;
}

void GetProfilerInfo(std::string& info);


} // namespace bbt::coroutine::detail
