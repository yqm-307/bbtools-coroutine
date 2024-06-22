#include <atomic>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>

namespace bbt::coroutine::detail
{ 

#ifdef BBT_COROUTINE_PROFILE
std::atomic_int Coroutine::m_created_size = 0;
std::atomic_int Coroutine::m_released_size = 0;
#endif

CoroutineId Coroutine::GenCoroutineId()
{
    static std::atomic_int _generate_id{BBT_COROUTINE_INVALID_COROUTINE_ID};
    return (++_generate_id);
}

Coroutine::SPtr Coroutine::Create(int stack_size, const CoroutineCallback& co_func, bool need_protect)
{
    return std::make_shared<Coroutine>(stack_size, co_func, need_protect);
}

Coroutine::SPtr Coroutine::Create(int stack_size, const CoroutineCallback& co_func, const CoroutineFinalCallback& co_final_cb, bool need_protect)
{
    return std::make_shared<Coroutine>(stack_size, co_func, co_final_cb, need_protect);
}

Coroutine::Coroutine(int stack_size, const CoroutineCallback& co_func, bool need_protect):
    m_context(stack_size, co_func, [this](){_OnCoroutineFinal();}, need_protect),
    m_id(GenCoroutineId())
{
    m_run_status = CoroutineStatus::CO_PENDING;
#ifdef BBT_COROUTINE_PROFILE
    m_created_size++;
#endif
}

Coroutine::Coroutine(int stack_size, const CoroutineCallback& co_func, const CoroutineFinalCallback& co_final_cb, bool need_protect):
    m_context(stack_size, co_func, [this](){_OnCoroutineFinal();}, need_protect),
    m_id(GenCoroutineId()),
    m_co_final_callback(co_final_cb)
{
    m_run_status = CoroutineStatus::CO_PENDING;
#ifdef BBT_COROUTINE_PROFILE
    m_created_size++;
#endif
}


Coroutine::~Coroutine()
{
#ifdef BBT_COROUTINE_PROFILE
    m_released_size++;
#endif
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
    if (m_co_final_callback)
        m_co_final_callback();
}

void Coroutine::BindProcesser(std::shared_ptr<Processer> processer)
{
    m_bind_processer_id = processer->GetId();
}

void Coroutine::OnEventTimeout(CoPollEvent::SPtr evnet)
{
    g_scheduler->OnActiveCoroutine(shared_from_this());
    _OnEventFinal();
}

void Coroutine::OnEventReadable(CoPollEvent::SPtr evnet)
{
    g_scheduler->OnActiveCoroutine(shared_from_this());
    _OnEventFinal();
}

void Coroutine::_OnEventFinal()
{
    if (m_readable_event != nullptr)
        m_readable_event->UnRegistEvent();
    m_readable_event = nullptr;

    if (m_timeout_event != nullptr)
        m_timeout_event->UnRegistEvent();
    m_timeout_event = nullptr;
}

ProcesserId Coroutine::GetBindProcesserId()
{
    return m_bind_processer_id;
}

std::shared_ptr<CoPollEvent> Coroutine::RegistTimeout(int ms)
{
    AssertWithInfo(m_timeout_event == nullptr, "不可以重复注册事件");
    m_timeout_event = CoPollEvent::Create(shared_from_this(), ms, [](CoPollEvent::SPtr event, Coroutine::SPtr co){
        co->OnEventTimeout(event);
    });

    Assert(m_timeout_event != nullptr);
    if (m_timeout_event->RegistEvent(0) != 0)
        return nullptr;
    
    return m_timeout_event;
}

std::shared_ptr<CoPollEvent> Coroutine::RegistReadable(int fd, int ms)
{
    return _RegistReadableEx(fd, ms, 0);
}

std::shared_ptr<CoPollEvent> Coroutine::RegistReadableET(int fd, int ms)
{
    return _RegistReadableEx(fd, ms, EPOLLET);
}

std::shared_ptr<CoPollEvent> Coroutine::_RegistReadableEx(int fd, int ms, int ext_event)
{
    AssertWithInfo(m_readable_event == nullptr, "不可以重复注册事件");
    m_readable_event = CoPollEvent::Create(shared_from_this(), fd, ms, [](CoPollEvent::SPtr event, Coroutine::SPtr co){
        co->OnEventReadable(event);
    });

    Assert(m_readable_event != nullptr);
    if (m_readable_event->RegistEvent(ext_event) != 0)
        return nullptr;

    return m_readable_event;
}

}