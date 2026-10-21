#include <bbt/core/util/Assert.hpp>
#include <bbt/coroutine/sync/CoMutex.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>
#include <bbt/coroutine/detail/DriverWakeDeferral.hpp>

namespace bbt::coroutine::sync
{

CoMutex::SPtr CoMutex::Create()
{
    return std::make_shared<CoMutex>(PrivateTag{});
}

CoMutex::CoMutex(PrivateTag)
{

}

CoMutex::~CoMutex()
{

}

void CoMutex::Lock()
{
    _SysLock();

    AssertWithInfo(g_bbt_tls_helper->EnableUseCo(), "must use in coroutine!");
    AssertWithInfo(m_locked_co != g_bbt_tls_coroutine_co, "comutex deadlock!");

    while (m_status == CoMutexStatus::COMUTEX_LOCKED) {
#if defined(BBT_COROUTINE_PROFILE)
        detail::Profiler::GetInstance()->OnEvent_CoMutexLockYield();
#endif
        /* #368：登记失败（ret<0）时挂起回调未执行、锁仍在手，不返回、
         * 不重入已持有的锁；释放 m_mutex 后经全局就绪队列让权——持锁
         * 方的 UnLock 需要同一把 m_mutex，且单 worker 下只有本协程让出
         * 处理器它才能运行——再重取锁复查 m_status，保持"返回即获锁"
         * 与持锁方最终可解锁的进展前提。失败推入的死事件留给下一次
         * _NotifyOne 经 Trigger()==-1 跳扫清理，与既有队列语义一致。
         * 本重试只在登记恢复后终止等待循环；持续登记失败时持续让权，
         * 不承诺通用持续 OOM 恢复。 */
        if (_WaitUnLock([this](){ _SysUnLock(); return true; }) == 0) {
            _SysLock();
            continue;
        }

        _SysUnLock();
        g_bbt_tls_coroutine_co->YieldAndPushGCoQueue();
        _SysLock();
    }

    _OnLocked();
    _SysUnLock();
}

void CoMutex::UnLock()
{
    /* _OnUnLocked→_NotifyOne→Trigger 的唤醒链在 m_mutex 内执行；
     * driver Wake 推迟到 _SysUnLock 之后（本函数退出时 flush）。 */
    detail::DriverWakeDeferral _defer;
    _SysLock();

    _OnUnLocked();

    _SysUnLock();
}

int CoMutex::TryLock()
{

    int ret = 0;
    _SysLock();
    AssertWithInfo(m_locked_co != g_bbt_tls_coroutine_co, "comutex deadlock!");

    if (m_status == CoMutexStatus::COMUTEX_FREE)
        _OnLocked();
    else
        ret = -1;

    _SysUnLock();
    return ret;
}

int CoMutex::TryLock(int ms)
{
    int ret = 0;
    _SysLock();
    AssertWithInfo(m_locked_co != g_bbt_tls_coroutine_co, "comutex deadlock!");

    if (m_status == CoMutexStatus::COMUTEX_LOCKED) {
        ret = _WaitUnLockUnitlTimeout(ms, [this](){ _SysUnLock(); return true; });

        /* #368：失败路径挂起回调未执行，锁仍在手不得重入；
         * 成功/超时路径回调已释放锁，必须重取后再看 m_status。 */
        if (bbt_unlikely(ret == -1)) {
            _SysUnLock();
            return -1;
        }

        _SysLock();
    }

    if (ret != 0) {
        _SysUnLock();
        return ret;
    }

    if (m_status != CoMutexStatus::COMUTEX_FREE) {
        _SysUnLock();
        return -1;
    }

    _OnLocked();
    _SysUnLock();
    return 0;
}

void CoMutex::_SysLock()
{
    m_mutex.lock();
}

void CoMutex::_SysUnLock()
{
    m_mutex.unlock();
}

int CoMutex::_WaitUnLockUnitlTimeout(int timeout, const detail::CoroutineOnYieldCallback& cb)
{
    auto* coroutine = g_bbt_tls_coroutine_co;
    AssertWithInfo(coroutine != nullptr, "current coroutine is nullptr!");
    /* #368-R1/F3：进入本函数时调用方仍持有 m_mutex，挂起回调（内含
     * _SysUnLock）负责在协程挂起后释放。等待准备阶段的一次分配失败
     * （RegistCustom 的 CoPollEvent 构造、队列 push、回调装配）会把异常
     * 抛出本函数——此时回调未执行、锁仍持有，catch 必须释放后重抛；
     * cb_ran 严格区分回调是否已执行过解锁，异常路径不得双解锁。
     * 清理本协程的 await 登记，避免已入队的死事件占用下一次等待。 */
    bool cb_ran = false;
    int ret = 0;
    std::shared_ptr<detail::CoPollEvent> event;
    try {
        event = coroutine->RegistCustom(detail::POLL_EVENT_CUSTOM_COMUTEX, timeout);
        if (event == nullptr)
            return -1;
        /* #368：事件入队前须保证唤醒可达——TryLock(-1) 语义是"等待至获锁或出错"，
         * 负 timeout 经 RegistCustom→InitFdEvent 拒收后若仍入队等待，协程将
         * 永久悬挂（无定时器），等价于 #198 式冻结；按无效期限立即报错。
         * 此时 await 登记已落在协程上，先清账再返回，避免下一次等待被拒。 */
        if (event->GetTimeout() < 0) {
            coroutine->_SetAwaitEvent(nullptr);
            return -1;
        }
        m_wait_event_queue.push(event);
        ret = coroutine->YieldWithCallback([coroutine, cb, &cb_ran](){
            if (!coroutine->_RegistAwaitEvent())
                return false;
            /* cb 一旦执行即已解锁 m_mutex（其语义就是挂起前放锁），
             * 先于其返回值置位，保证 catch 不重入已释放的锁。 */
            cb_ran = true;
            return cb();
        });
    } catch (...) {
        /* #368-R2/F3：callback 装配或 Context 存储分配失败时协程未挂起、
         * 事件从未注册。std::queue 无 pop_back——只清本事件自身槽位为
         * tombstone（不移动其它节点），_NotifyOne 消费时跳过 nullptr。
         * 直接 pop() 会误删队首的健康等待者；该事件此刻必在队尾（本
         * 临界区内只有本线程入队、协程未挂起无人出队），保守起见仅在
         * 确证仍是本次节点时置空。 */
        if (!cb_ran) {
            coroutine->_SetAwaitEvent(nullptr);
            if (event != nullptr && !m_wait_event_queue.empty() &&
                m_wait_event_queue.back() == event)
                m_wait_event_queue.back().reset();
            _SysUnLock();
        }
        throw;
    }

    /* #369：登记失败（ret != 0）时本协程未真正挂起，唤醒掩码仍是上一次
     * 等待的旧值，不得参与超时/取消映射——否则失败会被误报为超时 1。
     * #347② 兼容映射：协程级取消现以独立取消位唤醒（原借用超时位），
     * 本处把取消仍按超时上报 1，保持 TryLock(ms) 既有可观察行为不变。 */
    if (ret == 0 &&
        coroutine->GetLastResumeEvent() & (detail::POLL_EVENT_TIMEOUT | detail::POLL_EVENT_CANCELLED))
        return 1;  // timeout

    return ret;
}

int CoMutex::_WaitUnLock(const detail::CoroutineOnYieldCallback& cb)
{
    auto* coroutine = g_bbt_tls_coroutine_co;
    AssertWithInfo(coroutine != nullptr, "current coroutine is nullptr!");
    /* #368-R1/F3：与 _WaitUnLockUnitlTimeout 同一异常协议——等待准备的
     * 分配失败抛出时回调未执行、锁仍在手，释放后重抛并清 await 登记。 */
    bool cb_ran = false;
    std::shared_ptr<detail::CoPollEvent> event;
    try {
        event = coroutine->RegistCustom(detail::POLL_EVENT_CUSTOM_COMUTEX);
        if (event == nullptr)
            return -1;
        m_wait_event_queue.push(event);
        return coroutine->YieldWithCallback([coroutine, cb, &cb_ran](){
            if (!coroutine->_RegistAwaitEvent())
                return false;
            /* 同上：cb 一旦执行即已解锁，先于其返回值置位。 */
            cb_ran = true;
            return cb();
        });
    } catch (...) {
        /* #368-R2/F3：同 _WaitUnLockUnitlTimeout——未挂起的 stale 事件
         * 置 tombstone 由 _NotifyOne 跳过；pop() 会误删队首健康节点。 */
        if (!cb_ran) {
            coroutine->_SetAwaitEvent(nullptr);
            if (event != nullptr && !m_wait_event_queue.empty() &&
                m_wait_event_queue.back() == event)
                m_wait_event_queue.back().reset();
            _SysUnLock();
        }
        throw;
    }
}

void CoMutex::_NotifyOne()
{
    while (!m_wait_event_queue.empty()) {
        auto co_sptr = m_wait_event_queue.front();
        m_wait_event_queue.pop();
        /* tombstone（装配失败回滚置空的本轮事件）不占唤醒名额，直接跳过 */
        if (co_sptr == nullptr)
            continue;
        if (co_sptr->Trigger(detail::POLL_EVENT_CUSTOM) == 0)
            break;
    }
}

void CoMutex::_OnLocked()
{
    Assert(m_status == CoMutexStatus::COMUTEX_FREE);
    m_locked_co = g_bbt_tls_coroutine_co;
    m_status = CoMutexStatus::COMUTEX_LOCKED;
}

void CoMutex::_OnUnLocked()
{
    m_status = CoMutexStatus::COMUTEX_FREE;
    _NotifyOne();
    m_locked_co = nullptr;
}

}