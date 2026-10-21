#pragma once
#include <atomic>
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::detail
{

class DebugMgr
{
public:
    static std::unique_ptr<DebugMgr>& GetInstance();

    void OnEvent_ResumeCo(Coroutine* co) noexcept;
    /* 严格按 id 配对核销：erase==0 一律 AssertWithInfo 拦截，无任何缺口
     * 放行——ResumeCo 登记失败已由有界单次重试收口，不存在合法的无记录
     * YieldCo。 */
    void OnEvent_YieldCo(Coroutine* co) noexcept;
    /* 终态/销毁清账：协程对象析构时按 id 幂等移除借用记录。直接驱动
     * （Create/Resume/delete，不经 Processer）的 FINAL 让出走
     * Context::_CoroutineMain 尾部 Context::Yield，不经过
     * Coroutine::Yield/CommitYield，登记必须由销毁出口兜底；受管路径
     * 已在 CommitYield FINAL 核销，此处 erase 不到条目，无断言副作用。
     * 对称于事件侧 OnEvent_DestroyEvent 的「未登记即销毁同样合法」。 */
    void OnEvent_DestroyCo(CoroutineId id) noexcept;
    void Check_IsResumedCo(CoroutineId id);

    /* 登记成功返回 true；map 节点的单次分配失败返回 false（本函数内不抛出，
     * 已由 stderr 报告 + m_unhandled_exception_count 计数）。调用方负责按
     * 现有登记失败语义回收事件；事件以 shared_ptr 传入，失败时无任何 map
     * 残留——不引入「成功但漏记账」的第三态，配对不变式不受影响。 */
    bool TryRegistEvent(const std::shared_ptr<CoPollEvent>& event) noexcept;
    void OnEvent_TriggerEvent(std::shared_ptr<CoPollEvent> event);
    void OnEvent_DestroyEvent(CoPollEventId id);
    void Check_Trigger(CoPollEventId id);
private:
    /* 登记失败统一收口：stderr 报告 + m_unhandled_exception_count 计数，
     * 不静默丢失。可由 worker 原生栈调用，自身不得抛出。 */
    void ReportAllocFailed(const char* what, unsigned long long id,
                           const char* detail) noexcept;
    /* F-R3-04：m_co_map 登记的内部实现，只承担「find 判重 + insert 登记」
     * 本身；bad_alloc 时 std::map 提供强保证、map 无残留，调用方据此有界
     * 重试。不在此计数/报告——策略由唯一调用点 OnEvent_ResumeCo 决定。 */
    void RegistResumeCo(Coroutine* co);
    std::map<CoroutineId, Coroutine*>   m_co_map;
    std::mutex                                          m_co_map_mtx;

    /* 诊断不得延长等待对象寿命；真正析构时移除借用记录。 */
    std::map<CoPollEventId, std::weak_ptr<CoPollEvent>>     m_event_map;
    std::mutex                                              m_event_map_mtx;
};

} // namespace bbt::coroutine::detail