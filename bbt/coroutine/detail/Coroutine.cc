#include <atomic>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Processer.hpp>

namespace bbt::coroutine::detail
{

CoroutineId Coroutine::GenCoroutineId()
{
    static std::atomic_int _generate_id{BBT_COROUTINE_INVALID_COROUTINE_ID};
    return (++_generate_id);
}

Coroutine::SPtr Coroutine::Create(int stack_size, const CoroutineCallback& co_func, bool need_protect)
{
    return std::make_shared<Coroutine>(stack_size, co_func, need_protect);
}


Coroutine::Coroutine(int stack_size, const CoroutineCallback& co_func, bool need_protect):
    m_context(stack_size, co_func, [this](){_OnCoroutineFinal();}, need_protect),
    m_id(GenCoroutineId())
{
    m_run_status = CoroutineStatus::CO_PENDING;
}

Coroutine::~Coroutine()
{
}

void Coroutine::Resume()
{
    m_run_status = CoroutineStatus::CO_RUNNING;
    m_context.Resume();
}

void Coroutine::Yield()
{
    m_run_status = CoroutineStatus::CO_SUSPEND;
    m_context.Yield();
}

CoroutineId Coroutine::GetId()
{
    return m_id;
}

CoroutineStatus Coroutine::GetStatus()
{
    return m_run_status;
}

void Coroutine::_OnCoroutineFinal()
{
    m_run_status = CoroutineStatus::CO_FINAL;
}

void Coroutine::BindProcesser(std::shared_ptr<Processer> processer)
{
    m_bind_processer_id = processer->GetId();
}

void Coroutine::OnEventTimeout()
{
    g_scheduler->OnActiveCoroutine(shared_from_this());
}

ProcesserId Coroutine::GetBindProcesserId()
{
    return m_bind_processer_id;
}



}