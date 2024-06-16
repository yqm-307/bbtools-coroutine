#include <atomic>
#include <bbt/coroutine/detail/Coroutine.hpp>

namespace bbt::coroutine::detail
{

CoroutineId Coroutine::GenCoroutineId()
{
    static std::atomic_int _generate_id{BBT_COROUTINE_INVALID_COROUTINE_ID};
    return (++_generate_id);
}

Coroutine::SPtr Coroutine::Create(int stack_size, const CoroutineCallback& co_func, const CoroutineFinalCallback& co_final_cb, bool need_protect)
{
    return std::make_shared<Coroutine>(stack_size, co_func, co_final_cb, need_protect);
}


Coroutine::Coroutine(int stack_size, const CoroutineCallback& co_func, const CoroutineFinalCallback& co_final_cb, bool need_protect):
    m_context(stack_size, co_func, co_final_cb, need_protect),
    m_id(GenCoroutineId())
{
    m_run_status = CoroutineStatus::CO_PENDING;
}

Coroutine::~Coroutine()
{

}

void Coroutine::Resume()
{
    m_context.Resume();
}

void Coroutine::Yield()
{
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


}