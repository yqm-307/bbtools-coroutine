#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Processer.hpp>

namespace bbt::coroutine::detail
{

namespace {
// 平凡 TLS 观察槽不注册析构器；冷查询不触发 UPtr 的线程退出登记/分配。
thread_local LocalThread* g_local_thread{nullptr};
} // namespace

LocalThread::~LocalThread() noexcept {
    if (g_local_thread == this)
        g_local_thread = nullptr;
}

LocalThread::UPtr& LocalThread::GetTLSInst()
{
    static thread_local UPtr _tls;
    if (_tls == nullptr)
        _tls = UPtr(new LocalThread);

    g_local_thread = _tls.get();
    return _tls;
}

LocalThread* LocalThread::TryGetTLSInst() noexcept {
    return g_local_thread;
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