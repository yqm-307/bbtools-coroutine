#include <atomic>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/detail/Processer.hpp>

namespace bbt::coroutine::detail
{

Processer::SPtr Processer::Create()
{
    return std::make_shared<Processer>();
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
    return m_coroutine_queue.Size();
}

ProcesserId Processer::GetProcesserId()
{
    return m_id;
}

void Processer::AddCoroutineTask(Coroutine::SPtr coroutine)
{
    m_coroutine_queue.PushTail(coroutine);
    OnAddCorotinue();
}

void Processer::AddCoroutineTaskRange(std::vector<Coroutine::SPtr>::iterator begin, std::vector<Coroutine::SPtr>::iterator end)
{
    m_coroutine_queue.PushTailRange(begin, end);
    OnAddCorotinue();
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
        while(!m_coroutine_queue.Empty())
        {
            auto coroutine_sptr = m_coroutine_queue.PopHead();
            AssertWithInfo(coroutine_sptr != nullptr, "maybe coroutine queue has bug!");
            //TODO 不考虑执行一半，没有做挂起协程的唤醒机制
            coroutine_sptr->Resume();
        }
        std::unique_lock<std::mutex> lock_uptr(m_run_cond_mutex);
        m_run_status = ProcesserStatus::PROC_SUSPEND;
        m_run_cond.wait(lock_uptr);
    }

    m_run_status = ProcesserStatus::PROC_EXIT;
}

void Processer::Stop()
{
    m_is_running = false;
    while (m_run_status != ProcesserStatus::PROC_EXIT) {
        std::this_thread::sleep_for(bbt::clock::milliseconds(50));
        m_run_cond.notify_one();
    }
}

void Processer::OnAddCorotinue()
{
    // if (!m_is_running || !(m_run_status == ProcesserStatus::PROC_Suspend))
    if (!m_is_running)
        return;

    m_run_cond.notify_one();
}


} // namespace bbt::coroutine::detail
