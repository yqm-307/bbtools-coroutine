#include <atomic>
#include <bbt/coroutine/detail/Processer.hpp>

namespace bbt::coroutine::detail
{

ProcesserId Processer::_GenProcesserId()
{
    static std::atomic_uint64_t _generate_id{BBT_COROUTINE_INVALID_PROCESSER_ID};
    return (++_generate_id);
}

Processer::Processer():
    m_id(_GenProcesserId())
{
    m_run_status = ProcesserStatus::Suspend;
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

} // namespace bbt::coroutine::detail
