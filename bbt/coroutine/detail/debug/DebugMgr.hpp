#pragma once
#include <atomic>
#include <bbt/coroutine/detail/Define.hpp>

/** @file DebugMgr.hpp：严格调试模式的配对记账，resume/yield 与事件注册/触发必须成对，失配即断言。
 *  调用点都在 BBT_COROUTINE_STRINGENT_DEBUG 下，默认构建不调用，但本类始终编译。 */

namespace bbt::coroutine::detail {

/**
 * @brief 两张配对表的持有者：只登记、核销、断言，不参与调度决策；事件表存 weak_ptr，不延长等待对象寿命。
 *        记账分配失败走 ReportAllocFailed 报 stderr 并计入 m_unhandled_exception_count，不让异常逃出 worker 原生栈。
 */
class DebugMgr {
public:
    /* 进程内唯一实例，寿命与进程一致：刻意泄漏持有者，不参与静态析构 */
    static std::unique_ptr<DebugMgr>& GetInstance();

    /* Resume 登记：借用的 Coroutine* 按 id 记入 m_co_map，等 YieldCo 核销。
     * 在 worker 原生栈执行，自身不得抛出。 */
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
    /* 断言 id 没有未核销的 resume 登记：把协程重新交给队列前，上一次 Resume
     * 必须已经被 YieldCo 核销。 */
    void Check_IsResumedCo(CoroutineId id);

    /* 登记成功返回 true；map 节点的单次分配失败返回 false（本函数内不抛出，
     * 已由 stderr 报告 + m_unhandled_exception_count 计数）。调用方负责按
     * 现有登记失败语义回收事件；事件以 shared_ptr 传入，失败时无任何 map
     * 残留——不引入「成功但漏记账」的第三态，配对不变式不受影响。 */
    bool TryRegistEvent(const std::shared_ptr<CoPollEvent>& event) noexcept;
    /* 触发核销：按 id 摘掉登记并断言确实存在。以值收下 shared_ptr，核销期间
     * 事件对象由本栈保活，不受事件表只存 weak_ptr 影响。 */
    void OnEvent_TriggerEvent(std::shared_ptr<CoPollEvent> event);
    void OnEvent_DestroyEvent(CoPollEventId id);
    /* 断言 id 仍有一条未触发的登记，与 OnEvent_TriggerEvent 的核销配对 */
    void Check_Trigger(CoPollEventId id);

private:
    /* 登记失败统一收口：stderr 报告 + m_unhandled_exception_count 计数，
     * 不静默丢失。可由 worker 原生栈调用，自身不得抛出。 */
    void ReportAllocFailed(const char* what, unsigned long long id, const char* detail) noexcept;
    /* m_co_map 登记的内部实现，只承担「find 判重 + insert 登记」
     * 本身；bad_alloc 时 std::map 提供强保证、map 无残留，调用方据此有界
     * 重试。不在此计数/报告——策略由唯一调用点 OnEvent_ResumeCo 决定。 */
    void RegistResumeCo(Coroutine* co);
    std::map<CoroutineId, Coroutine*> m_co_map;
    std::mutex m_co_map_mtx;

    /* 诊断不得延长等待对象寿命；真正析构时移除借用记录。 */
    std::map<CoPollEventId, std::weak_ptr<CoPollEvent>> m_event_map;
    std::mutex m_event_map_mtx;
};

} // namespace bbt::coroutine::detail