#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Processer.hpp>

namespace bbt::coroutine::detail
{

LocalThread::UPtr& LocalThread::GetTLSInst()
{
    static thread_local UPtr _tls = nullptr;
    if (_tls == nullptr)
        _tls = UPtr(new LocalThread);

    return _tls;
}


std::shared_ptr<Processer> LocalThread::GetProcesser()
{
    if (m_enable_use_co)
        return Processer::GetLocalProcesser();
    
    return nullptr;
}

void LocalThread::SetEnableUseCo(bool enable_use_coroutine)
{
    m_enable_use_co = enable_use_coroutine;
}

bool LocalThread::EnableUseCo()
{
    return m_enable_use_co;
}


}