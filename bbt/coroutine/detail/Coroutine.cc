#include <atomic>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>

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
    g_bbt_profiler->OnEvent_CreateCoroutine();
#endif
}

Coroutine::Coroutine(int stack_size, const CoroutineCallback& co_func, const CoroutineFinalCallback& co_final_cb, bool need_protect):
    m_context(stack_size, co_func, [this](){_OnCoroutineFinal();}, need_protect),
    m_id(GenCoroutineId()),
    m_co_final_callback(co_final_cb)
{
    m_run_status = CoroutineStatus::CO_PENDING;
#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_CreateCoroutine();
#endif
}


Coroutine::~Coroutine()
{
    // Assert(m_bind_processer_id != BBT_COROUTINE_INVALID_PROCESSER_ID);
#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_DestoryCoroutine();
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

// void Coroutine::OnEventTimeout(CoPollEvent::SPtr evnet)
// {
//     g_scheduler->OnActiveCoroutine(shared_from_this());
//     _OnEventFinal();
// }

// void Coroutine::OnEventReadable(CoPollEvent::SPtr evnet)
// {
//     g_scheduler->OnActiveCoroutine(shared_from_this());
//     _OnEventFinal();
// }

// void Coroutine::OnEventChanWrite()
// {
//     g_scheduler->OnActiveCoroutine(shared_from_this());
// }


// void Coroutine::_OnEventFinal()
// {
// }

std::shared_ptr<CoPollEvent> Coroutine::RegistTimeout(int ms)
{
    std::unique_lock<std::mutex> _(m_await_event_mutex);
    if (m_await_event != nullptr)
        return nullptr;

    m_await_event = CoPollEvent::Create(shared_from_this(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });

    if (m_await_event->InitTimeoutEvent(ms) != 0)
        return nullptr;

    if (m_await_event->Regist() != 0)
        return nullptr;

    return m_await_event;
}

std::shared_ptr<CoPollEvent> Coroutine::RegistCustom(int key)
{
    std::unique_lock<std::mutex> _(m_await_event_mutex);
    if (m_await_event != nullptr)
        return nullptr;
    
    m_await_event = CoPollEvent::Create(shared_from_this(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });

    if (m_await_event->InitCustomEvent(key, NULL) != 0)
        return nullptr;
    
    if (m_await_event->Regist() != 0)
        return nullptr;
    
    return m_await_event;
}

std::shared_ptr<CoPollEvent> Coroutine::RegistCustom(int key, int timeout_ms)
{
    std::unique_lock<std::mutex> _(m_await_event_mutex);
    if (m_await_event != nullptr)
        return nullptr;
    
    m_await_event = CoPollEvent::Create(shared_from_this(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });

    if (m_await_event->InitCustomEvent(key, NULL) != 0)
        return nullptr;
    
    if (m_await_event->InitTimeoutEvent(timeout_ms) != 0)
        return nullptr;
    
    if (m_await_event->Regist() != 0)
        return nullptr;
    
    return m_await_event;
}

std::shared_ptr<CoPollEvent> Coroutine::RegistFdReadable(int fd)
{
    std::unique_lock<std::mutex> _(m_await_event_mutex);
    if (m_await_event != nullptr)
        return nullptr;
    
    m_await_event = CoPollEvent::Create(shared_from_this(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });

    if (m_await_event->InitFdReadableEvent(fd) != 0)
        return nullptr;
    
    if (m_await_event->Regist() != 0)
        return nullptr;
    
    return m_await_event;
}

std::shared_ptr<CoPollEvent> Coroutine::RegistFdReadable(int fd, int timeout_ms)
{
    std::unique_lock<std::mutex> _(m_await_event_mutex);
    if (m_await_event != nullptr)
        return nullptr;
    
    m_await_event = CoPollEvent::Create(shared_from_this(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });

    if (m_await_event->InitFdReadableEvent(fd) != 0)
        return nullptr;
    
    if (m_await_event->InitTimeoutEvent(timeout_ms) != 0)
        return nullptr;
    
    if (m_await_event->Regist() != 0)
        return nullptr;
    
    return m_await_event;
}

std::shared_ptr<CoPollEvent> Coroutine::RegistFdWriteable(int fd)
{
    std::unique_lock<std::mutex> _(m_await_event_mutex);
    if (m_await_event != nullptr)
        return nullptr;
    
    m_await_event = CoPollEvent::Create(shared_from_this(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });

    if (m_await_event->InitFdWriteableEvent(fd) != 0)
        return nullptr;
    
    if (m_await_event->Regist() != 0)
        return nullptr;
    
    return m_await_event;
}

std::shared_ptr<CoPollEvent> Coroutine::RegistFdWriteable(int fd, int timeout_ms)
{
    std::unique_lock<std::mutex> _(m_await_event_mutex);
    if (m_await_event != nullptr)
        return nullptr;
    
    m_await_event = CoPollEvent::Create(shared_from_this(), [this](auto, int event, int custom_key){
        OnCoPollEvent(event, custom_key);
    });

    if (m_await_event->InitFdWriteableEvent(fd) != 0)
        return nullptr;
    
    if (m_await_event->InitTimeoutEvent(timeout_ms) != 0)
        return nullptr;

    if (m_await_event->Regist() != 0)
        return nullptr;
    
    return m_await_event;
}

void Coroutine::OnCoPollEvent(int event, int custom_key)
{
    std::unique_lock<std::mutex> _(m_await_event_mutex);
    Assert(m_await_event != nullptr);

    g_scheduler->OnActiveCoroutine(shared_from_this());

    m_await_event = nullptr;
}


}