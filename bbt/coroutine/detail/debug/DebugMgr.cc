#include <bbt/coroutine/detail/debug/DebugMgr.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <cstdio>


namespace bbt::coroutine::detail
{

std::unique_ptr<DebugMgr>& DebugMgr::GetInstance()
{
    /* 进程寿命：运行期对象，不在静态退出期析构 */
    static std::unique_ptr<DebugMgr>* _holder =
        new std::unique_ptr<DebugMgr>(new DebugMgr());
    
    return *_holder;
}

void DebugMgr::ReportAllocFailed(const char* what, unsigned long long id,
                                 const char* detail) noexcept
{
    std::fprintf(stderr, "[bbtco] stringent-debug %s alloc failed id=%llu: %.200s\n",
                 what, id, detail);
    g_bbt_coroutine_config->m_unhandled_exception_count.fetch_add(1, std::memory_order_relaxed);
}

void DebugMgr::RegistResumeCo(Coroutine* co)
{
    std::lock_guard<std::mutex> _(m_co_map_mtx);
    AssertWithInfo(m_co_map.find(co->GetId()) == m_co_map.end(), "[error] repeat resume coroutine!");
    m_co_map.insert(std::make_pair(co->GetId(), co));
}

void DebugMgr::OnEvent_ResumeCo(Coroutine* co) noexcept
{
    /* 登记在 worker 原生栈（Processer::Resume）执行，异常出逃
     * 即 terminate，协程侧 catch 不可达——本函数必须完全吞住。
     * insert 失败时 std::map 强保证 map 无残留，而登记操作本身
     * 无副作用，单次分配失败后有界重试一次真实登记即可恢复按 id 的精确
     * 条目；连续失败不承诺自愈（报告+计数后返回，协程继续），但不允许
     * 无界循环/跳诊断/抛出跨 worker。 */
    try
    {
        RegistResumeCo(co);
        return;
    }
    catch (const std::bad_alloc& e)
    {
        ReportAllocFailed("resume-co", co->GetId(), e.what());
    }
    catch (const std::exception& e)
    {
        ReportAllocFailed("resume-co", co->GetId(), e.what());
        return;
    }
    catch (...)
    {
        ReportAllocFailed("resume-co", co->GetId(), "unknown exception");
        return;
    }

    /* 单次 bad_alloc 后的有界重试：同一无副作用登记再做一次。 */
    try
    {
        RegistResumeCo(co);
    }
    catch (const std::exception& e)
    {
        ReportAllocFailed("resume-co", co->GetId(), e.what());
    }
    catch (...)
    {
        ReportAllocFailed("resume-co", co->GetId(), "unknown exception");
    }
}

void DebugMgr::OnEvent_YieldCo(Coroutine* co) noexcept
{
    /* 严格按 id 配对核销：erase==0 一律 AssertWithInfo 拦截，无任何缺口
     * 放行——ResumeCo 登记失败已由有界单次重试收口，不存在合法的无记录
     * YieldCo。 */
    std::lock_guard<std::mutex> _(m_co_map_mtx);
    AssertWithInfo(m_co_map.erase(co->GetId()) > 0, "[error] repeat yield coroutine!");
}

void DebugMgr::OnEvent_DestroyCo(CoroutineId id) noexcept
{
    /* 幂等清账：受管 FINAL 已在 CommitYield 核销，找不到条目是正常；
     * 直接驱动的 FINAL/中途销毁在此兜底，禁止诊断表残留悬空指针。
     * erase 不分配，本函数天然 noexcept。ResumeCo 的 OOM 缺口由首个
     * 配对失败的 YieldCo 消费；销毁不消费缺口——受管 FINAL 在
     * CommitYield 已正常核销（erase==0 属常态），在此消费会误吞
     * 其它协程的未消化缺口。 */
    std::lock_guard<std::mutex> _(m_co_map_mtx);
    m_co_map.erase(id);
}

void DebugMgr::Check_IsResumedCo(CoroutineId id)
{
    std::lock_guard<std::mutex> _(m_co_map_mtx);
    Assert(m_co_map.find(id) == m_co_map.end());
}

bool DebugMgr::TryRegistEvent(const std::shared_ptr<CoPollEvent>& event) noexcept
{
    try
    {
        std::lock_guard<std::mutex> _(m_event_map_mtx);
        AssertWithInfo(m_event_map.find(event->GetId()) == m_event_map.end(), "[error] repeat regist event!");
        m_event_map.insert(std::make_pair(event->GetId(), event));
        return true;
    }
    catch (const std::exception& e)
    {
        /* map 节点的单次分配失败不得逃出本函数——调用点可位于
         * Context::Resume 的原生 worker 栈（挂起后注册回调），协程侧 catch
         * 不可达，出逃即 terminate。诊断清账按运行时契约的日志 + 计数口径交付，
         * 不静默丢失；登记语义由调用方按「建立失败」回收，map 无任何残留。 */
        std::fprintf(stderr, "[bbtco] stringent-debug regist-event alloc failed event=%llu: %.200s\n",
                     static_cast<unsigned long long>(event->GetId()), e.what());
        g_bbt_coroutine_config->m_unhandled_exception_count.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    catch (...)
    {
        std::fprintf(stderr, "[bbtco] stringent-debug regist-event alloc failed event=%llu: unknown exception\n",
                     static_cast<unsigned long long>(event->GetId()));
        g_bbt_coroutine_config->m_unhandled_exception_count.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

void DebugMgr::OnEvent_TriggerEvent(std::shared_ptr<CoPollEvent> event)
{
    std::lock_guard<std::mutex> _(m_event_map_mtx);
    AssertWithInfo(m_event_map.erase(event->GetId()) > 0, "[error] trigger event no registed or triggered!");
}

void DebugMgr::OnEvent_DestroyEvent(CoPollEventId id)
{
    std::lock_guard<std::mutex> _(m_event_map_mtx);
    m_event_map.erase(id); // 未登记即销毁、已完成后销毁同样合法。
}

void DebugMgr::Check_Trigger(CoPollEventId id)
{
    std::lock_guard<std::mutex> _(m_event_map_mtx);
    Assert(m_event_map.find(id) != m_event_map.end());
}

} // namespace bbt::coroutine::detail