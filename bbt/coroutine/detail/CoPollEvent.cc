#include <cstdio>
#include <fcntl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <exception>
#include <string>
#include <bbt/core/util/Assert.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/core/log/DebugPrint.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/utils/DebugPrint.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>
#include <bbt/coroutine/detail/debug/DebugMgr.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>

namespace bbt::coroutine::detail
{

namespace
{

void ReportCoPollEventException(CoroutineId co_id, CoPollEventId event_id,
                                std::exception_ptr eptr) noexcept
{
    const auto report = [co_id, event_id](const char* message) noexcept {
        std::fprintf(stderr, "[bbtco] unhandled coevent exception co=%llu event=%llu: %.200s\n",
                     static_cast<unsigned long long>(co_id),
                     static_cast<unsigned long long>(event_id), message);
        g_bbt_coroutine_config->m_unhandled_exception_count.fetch_add(1, std::memory_order_relaxed);
    };

    try
    {
        if (eptr != nullptr)
            std::rethrow_exception(eptr);
    }
    catch (const std::exception& e)
    {
        report(e.what());
        return;
    }
    catch (...)
    {
    }

    report("unknown exception");
}

/* #284：重试等待必须直达内核。std::this_thread::sleep_for 经 libc nanosleep，
 * 而本进程 nanosleep 已被 Hook 拦截——协程上下文会重入 YieldUntilTimeout，
 * 造成 double-wait 断言与等待状态错乱；syscall(2) 绕过全部 userspace hook。 */
void RawSleepMs(long ms) noexcept
{
    struct timespec req{ms / 1000, (ms % 1000) * 1000000L};
    while (::syscall(SYS_nanosleep, &req, &req) != 0 && errno == EINTR)
    {
    }
}

}

int TransformToPollEventType(short pollevent_type, bool has_custom)
{
    int ret = 0x0;
    if (pollevent_type & pollevent::EventOpt::READABLE)
        ret |= PollEventType::POLL_EVENT_READABLE;

    if (pollevent_type & pollevent::EventOpt::WRITEABLE)
        ret |= PollEventType::POLL_EVENT_WRITEABLE;

    if (pollevent_type & pollevent::EventOpt::TIMEOUT)
        ret |= PollEventType::POLL_EVENT_TIMEOUT;

    if (has_custom)
        ret |= PollEventType::POLL_EVENT_CUSTOM;

    return ret;
}

CoPollEvent::SPtr CoPollEvent::Create(CoroutineId id, const CoPollEventCallback& cb)
{
    return std::make_shared<CoPollEvent>(id, cb);
}

CoPollEvent::CoPollEvent(CoroutineId id, const CoPollEventCallback& cb):
    m_co_id(id),
    m_event_id(_GenerateId()),
    m_onevent_callback(cb)
{
    Assert(m_onevent_callback != nullptr);
}

CoPollEvent::~CoPollEvent()
{
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->OnEvent_DestroyEvent(m_event_id);
#endif
    // 已 arm 的 Event 只能在 PollOnce 线程析构，避免与 event loop 竞态。
    _CannelAllFdEvent();
}

int CoPollEvent::Trigger(short trigger_events)
{
    if (trigger_events == 0)
        return -1;

    uint64_t state = m_state.load(std::memory_order_acquire);
    for (;;)
    {
        const auto phase = GetCoPollEventPhase(state);
        if (phase == CoPollEventPhase::INITED ||
            phase == CoPollEventPhase::ARMING ||
            phase == CoPollEventPhase::ARMED)
        {
            const uint64_t pending = PackCoPollEventState(CoPollEventPhase::PENDING, trigger_events);
            if (m_state.compare_exchange_weak(state, pending,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire))
                return 0;
            continue;
        }

        if (phase != CoPollEventPhase::PARKED)
            return -1;

        const uint64_t triggering = PackCoPollEventState(CoPollEventPhase::TRIGGERING,
                                                           trigger_events);
        if (m_state.compare_exchange_weak(state, triggering,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire))
        {
            try
            {
                return _Complete(trigger_events);
            }
            catch (...)
            {
                ReportCoPollEventException(m_co_id, m_event_id, std::current_exception());
                return 0;
            }
        }
    }
}

CoPollEventId CoPollEvent::_GenerateId()
{
    static std::atomic_uint64_t _id{0};
    return ++_id;
}

/* fd → {代际, closing, 活跃等待事件}（#262 唤醒 + #370 代际线性化）
 *
 * 进程寿命持有：本表与互斥量被 worker / poller 在进程寿命内持续可达
 * （worker 活到进程结束，没有 Stop/join 可等待）。若它们参与静态析构，
 * 正常退出时注册表的析构与在途 worker 的表访问构成竞态窗口。
 * 与 Scheduler / GlobalConfig / CoPoller / StackPool / Profiler 五个单例
 * 同一策略：只泄漏、不在静态退出期析构（退出由 OS 回收）。
 * 用函数局部静态保证首次使用时初始化，避免动态初始化顺序问题。 */
std::mutex& CoPollEvent::_WaitersMtx()
{
    static std::mutex* mtx = new std::mutex();
    return *mtx;
}

CoPollEvent::FdSlots& CoPollEvent::_Slots()
{
    static FdSlots* slots = new FdSlots();
    return *slots;
}

bool CoPollEvent::_TrackWaiter()
{
    if (m_fd < 0 || m_event == nullptr)
        return true;    // 非 fd 事件：不登记，也无须代际判定

    std::lock_guard<std::mutex> lock(_WaitersMtx());
    auto& slot = _Slots()[m_fd];
    /* #370 建立窗口闭合：登记与采样虽然仍在同一临界区，但判定基线是
     * InitFdEvent 时锁内采样的 establish-epoch（m_wait_epoch），而非
     * 此刻的 slot.epoch。两种失败形态：
     *   - slot.closing：close 已声明（BeginFdClose 摘表、bump 完成），
     *     底层 close 未返回，fd 数字随时复用；
     *   - slot.epoch != m_wait_epoch：close 完整落在「建立等待→登记」
     *     窗口内（asio async_wait 已在旧 fd 上 arm 但底层 fd 已死），
     *     或建立期间又经历了多次 close+reuse。
     * 二者都意味着本次等待已不可能被正确事件唤醒——拒绝登记，由
     * Regist 以 POLL_EVENT_CLOSED 自触，恢复侧按 EBADF 收敛。 */
    if (slot.closing || slot.epoch != m_wait_epoch)
        return false;
    auto& vec = slot.waiters;
    vec.push_back(weak_from_this());
    /* 惰性清理仅在「从未发生过 close」的槽位上进行：epoch>0 的槽位携带
     * close 历史，必须由 FdEpoch 永久可查——否则「触发后未恢复」窗口内
     * 的 close 会把 epoch 抹回 0，与首代 establish-epoch 别名（#370
     * round-2 复审发现）。closing 槽位由 EndFdClose 收尾统一清理。 */
    if (vec.size() > 16) {
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                                 [](const std::weak_ptr<CoPollEvent>& w) { return w.expired(); }),
                  vec.end());
        if (vec.empty() && !slot.closing && slot.epoch == 0)
            _Slots().erase(m_fd);
    }
    return true;
}

void CoPollEvent::_UntrackWaiter()
{
    if (m_fd < 0)
        return;

    std::lock_guard<std::mutex> lock(_WaitersMtx());
    auto it = _Slots().find(m_fd);
    if (it == _Slots().end())
        return;

    auto& vec = it->second.waiters;
    vec.erase(std::remove_if(vec.begin(), vec.end(),
                             [this](const std::weak_ptr<CoPollEvent>& w) {
                                 auto p = w.lock();
                                 return p.get() == this || p == nullptr;
                             }),
              vec.end());
    /* epoch>0 的槽位携带 close 历史不得擦除：FdEpoch 对缺席槽位返回 0，
     * 会与「从未等待过该 fd」的首代 establish-epoch 0 别名，使恢复路径
     * FdWaitEpochValid 误判合法（#370 round-2 复审发现）。closing 槽位
     * 由 EndFdClose 收尾；仅 epoch==0（从未 close）且无 waiter 才摘除。 */
    if (vec.empty() && !it->second.closing && it->second.epoch == 0)
        _Slots().erase(it);
}

uint64_t CoPollEvent::FdEpoch(int fd)
{
    std::lock_guard<std::mutex> lock(_WaitersMtx());
    auto it = _Slots().find(fd);
    return it == _Slots().end() ? 0 : it->second.epoch;
}

size_t CoPollEvent::FdWaiterCount(int fd)
{
    std::lock_guard<std::mutex> lock(_WaitersMtx());
    auto it = _Slots().find(fd);
    if (it == _Slots().end())
        return 0;
    size_t n = 0;
    for (auto& w : it->second.waiters)
        if (!w.expired())
            ++n;
    return n;
}

bool CoPollEvent::BeginFdClose(int fd)
{
    std::vector<SPtr> snapshot;
    {
        std::lock_guard<std::mutex> lock(_WaitersMtx());
        /* #370 线性化点：在底层 close 之前，于同一临界区内完成
         * 代际推进 + closing 置位 + 摘走全部旧 waiter。此后到达的同号
         * waiter 采样新 epoch 或看到 closing，均不会被本批误摘/误醒。 */
        auto& slot = _Slots()[fd];
        if (slot.closing)
            return false;   // 已被其它线程 claim；本 close 走 EBADF/原语义
        // 所有可能分配必须先完成；准备失败不发布 closing/新 epoch，旧 waiter 仍在表内。
        snapshot.reserve(slot.waiters.size());
        for (auto& w : slot.waiters)
            if (auto p = w.lock())
                snapshot.push_back(p);
        ++slot.epoch;
        slot.closing = true;
        slot.waiters.clear();
    }
    // 锁外 Trigger：避免与完成路径的加锁顺序纠缠
    for (auto& p : snapshot)
        p->Trigger(POLL_EVENT_CLOSED);
    return true;
}

void CoPollEvent::EndFdClose(int fd)
{
    /* 底层 close 已返回（成功或失败），解除 closing；此后登记的同号 waiter
     * 属于复用后的新对象，采样新一代际。close 失败时旧对象仍存在，但代际
     * 已推进——保守地把同号 fd 视为新一代，旧 waiter 已摘走不重挂。 */
    std::lock_guard<std::mutex> lock(_WaitersMtx());
    auto it = _Slots().find(fd);
    if (it == _Slots().end())
        return;
    it->second.closing = false;
    /* epoch>0 的槽位必须保留：FdEpoch 缺席返回 0 会与首代 establish-epoch
     * 别名，使「触发后未恢复」窗口内的 close 无法被识别（#370 round-2）。 */
    if (it->second.waiters.empty() && it->second.epoch == 0)
        _Slots().erase(it);
}

bool CoPollEvent::WaitFdCloseReady(int fd, int timeout_ms)
{
    /* 自旋有界等待 claim 方 EndFdClose；窗口常态为微秒级（一个 syscall），
     * 1ms 粒度足够。槽位缺席 ⇒ closing 已解除或本 fd 从未被 claim，均安全。 */
    for (int i = 0; i < timeout_ms; ++i) {
        {
            std::lock_guard<std::mutex> lock(_WaitersMtx());
            auto it = _Slots().find(fd);
            if (it == _Slots().end() || !it->second.closing)
                return true;
        }
        RawSleepMs(1);
    }
    std::lock_guard<std::mutex> lock(_WaitersMtx());
    auto it = _Slots().find(fd);
    return it == _Slots().end() || !it->second.closing;
}

bool CoPollEvent::FdIsClosing(int fd)
{
    std::lock_guard<std::mutex> lock(_WaitersMtx());
    auto it = _Slots().find(fd);
    return it != _Slots().end() && it->second.closing;
}

int CoPollEvent::InitFdEvent(int fd, short events, int timeout)
{
    if (GetCoPollEventPhase(m_state.load(std::memory_order_acquire)) != CoPollEventPhase::INITED ||
        timeout < 0 || m_event != nullptr)
        return -1;

    m_fd = fd;
    m_listen_events = events;
    m_timeout = timeout;
    /* #370 establish-epoch + #402 establish-closing：在绑 fd 建立等待的起点
     * 于同一临界区采样 fd 代际与 closing。
     *   - m_wait_epoch 作为本次等待的代际基线：_TrackWaiter 登记时若发现
     *     epoch 已推进（InitFdEvent→Regist 窗口内发生过 close），即拒绝
     *     登记，由 Regist 以 POLL_EVENT_CLOSED 自触——close 已完整落在建立
     *     窗口内，本次等待不能再被当作有效等待。
     *   - establish_closing 覆盖更靠前的窗口：BeginFdClose 已推进代际并
     *     置 closing，但底层 close 尚未返回。若在此窗口内建立等待，Init
     *     采到的已是新一代 epoch；EndFdClose 解除 closing 后，登记时
     *     _TrackWaiter 看到「epoch 相同且未 closing」会误当新对象准入。
     *     这条等待并不会安静失效：Event 构造期的私有 dup 若早于用户
     *     close，旧 open file description 经该 dup 存活，旧 peer 就绪仍会
     *     真的唤醒本事件；若 dup 落在 close+reuse 之后，则直接绑到同号新
     *     对象上。两种形态在同一 epoch 下都被放行，风险是漏关闭（本应随
     *     close 收敛 EBADF 的等待被当成新代有效等待）与跨对象重试（唤醒
     *     后 syscall 打到同号新对象）。故建立时只要观察到 closing，就不
     *     创建底层 Event、把失效结果锁存为 POLL_EVENT_CLOSED（INITED→
     *     PENDING，复用既有提前首胜），不留到 Regist 时重获有效性；恢复
     *     侧由 FdWaitEpochValid 读 CLOSED 位收敛 EBADF，与「等待中被摘表」
     *     走同一路径。 */
    bool establish_closing = false;
    if (fd >= 0) {
        std::lock_guard<std::mutex> lock(_WaitersMtx());
        const auto& slot = _Slots()[fd];
        m_wait_epoch = slot.epoch;
        establish_closing = slot.closing;
    }
    /* establish 命中 closing：不再创建底层 Event——其构造期 dup 可能落在
     * 已复用的同号新对象上，且本次等待已被判定为失效（不再有资格成为新代
     * 有效等待）。Init 仍返回 0（等待建立本身成功），失效经锁存的
     * PENDING(CLOSED) 走既有完成/EBADF 路径；若其它触发已先胜（如取消），
     * Trigger 失败无害，首胜结果不变。 */
    if (establish_closing) {
        Trigger(POLL_EVENT_CLOSED);
        return 0;
    }
    auto weakthis = weak_from_this();
    /* #284：asio 侧旧监听的析构延迟到驱动线程下一次 PollOnce 兑现；本协程
     * 被唤醒后若抢在兑现前为同一 fd 建新监听，epoll ADD 报 EEXIST，异常曾
     * 一路逃出杀死连接协程（客户端超时）。此处有界重试：驱动约 1ms 一拍，
     * 旧监听很快释放；仍失败则返回 -1 不抛异常，调用方按等待建立失败处理，
     * 协程存活。常态零开销，仅竞态路径短睡 worker（默认 stall 告警关闭）。 */
    for (int attempt = 0; ; ++attempt)
    {
        try
        {
            m_event = g_bbt_poller->CreateEvent(fd, events, [weakthis](int fd, short events, bbt::pollevent::EventId eventid){
                if (weakthis.expired()) return;
                auto pthis = weakthis.lock();
                if (pthis == nullptr) return;
                pthis->Trigger(events);
            });
            break;
        }
        catch (const std::exception& e)
        {
            if (attempt >= 3)
            {
                std::fprintf(stderr, "[bbtco] InitFdEvent give up co=%llu fd=%d: %.100s\n",
                             static_cast<unsigned long long>(m_co_id), fd, e.what());
                return -1;
            }
            RawSleepMs(1);
        }
    }

    return m_event == nullptr ? -1 : 0;
}

int CoPollEvent::InitCustomEvent(int key, void* args)
{
    BBTATTR_COMM_UNUSED void* unused_args = args;
    if (GetCoPollEventPhase(m_state.load(std::memory_order_acquire)) != CoPollEventPhase::INITED ||
        m_has_custom_event)
        return -1;

    m_has_custom_event = true;
    m_custom_key = key;
    return 0;
}

int CoPollEvent::Regist()
{
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    /* R2-02：诊断登记经 TryRegistEvent 收口——map 节点单次分配失败返回
     * false（内部已报告 + 计数，不抛出——本函数可运行在 Context::Resume
     * 原生 worker 栈，协程侧 catch 不可达，出逃即 terminate）。
     * 失败语义统一为「登记未完成」：PENDING 分支返回 -1 由挂起回调按
     * CHECK_FAILED 收口（CommitPark 不再消费，事件不走向 _Complete，
     * 无 map 配对需求）；ARMED 主线先 UnRegist 收敛终态再 -1。
     * 登记点仍在原分支内：失败不提前登记，保证同一事件的多次 Regist
     * 调用与「repeat regist」断言语义不变。
     * F-01：std::map::insert 单次分配失败无副作用（强保证 map 无残留），
     * 同一无副作用登记可有界重试一次——与 co 侧 OnEvent_ResumeCo 同一
     * 策略，保住已接纳的 PENDING 首胜（触发位与 Notify 载荷）。连续 OOM
     * 第二次失败仍按「登记未完成」返回 false（TryRegistEvent 内部已各自
     * 报告 + 计数），不承诺持续 OOM 自愈；无无界循环、无跳记账、无手工
     * 再次 Trigger。 */
    const auto dbg_regist = [this]() -> bool {
        if (g_bbt_dbgmgr->TryRegistEvent(shared_from_this()))
            return true;
        return g_bbt_dbgmgr->TryRegistEvent(shared_from_this());
    };
#else
    const auto dbg_regist = []() -> bool { return true; };
#endif

    uint64_t state = m_state.load(std::memory_order_acquire);
    if (GetCoPollEventPhase(state) == CoPollEventPhase::PENDING) {
        return dbg_regist() ? 0 : -1;
    }

    uint64_t expected = PackCoPollEventState(CoPollEventPhase::INITED);
    if (!m_state.compare_exchange_strong(expected, PackCoPollEventState(CoPollEventPhase::ARMING),
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire))
    {
        if (GetCoPollEventPhase(expected) == CoPollEventPhase::PENDING) {
            return dbg_regist() ? 0 : -1;
        }
        return -1;
    }

    auto keep_alive = shared_from_this();
    int regist_ret = 0;
    try
    {
        if (m_event != nullptr)
            regist_ret = _RegistFdEvent();
    }
    catch (...)
    {
        // StartListen() 抛出时仍由 ARMING 持有者收敛状态并转移 Event。
        state = m_state.load(std::memory_order_acquire);
        while (GetCoPollEventPhase(state) == CoPollEventPhase::ARMING) {
            if (m_state.compare_exchange_weak(state, PackCoPollEventState(CoPollEventPhase::CANCELLED),
                                              std::memory_order_acq_rel,
                                               std::memory_order_acquire))
                break;
        }
        _CannelAllFdEvent();
        if (GetCoPollEventPhase(state) == CoPollEventPhase::PENDING) {
            return dbg_regist() ? 0 : -1;
        }
        return -1;
    }

    if (regist_ret != 0) {
        state = m_state.load(std::memory_order_acquire);
        while (GetCoPollEventPhase(state) == CoPollEventPhase::ARMING) {
            if (m_state.compare_exchange_weak(state, PackCoPollEventState(CoPollEventPhase::CANCELLED),
                                              std::memory_order_acq_rel,
                                               std::memory_order_acquire))
                break;
        }
        _CannelAllFdEvent();
        if (GetCoPollEventPhase(state) == CoPollEventPhase::PENDING) {
            return dbg_regist() ? 0 : -1;
        }
        return -1;
    }

    expected = PackCoPollEventState(CoPollEventPhase::ARMING);
    if (!m_state.compare_exchange_strong(expected, PackCoPollEventState(CoPollEventPhase::ARMED),
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire))
    {
        _CannelAllFdEvent();
        if (GetCoPollEventPhase(expected) == CoPollEventPhase::PENDING) {
            return dbg_regist() ? 0 : -1;
        }
        return -1;
    }

#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    /* 登记拒绝也会进入 CLOSED 完成路径，必须先记账再允许它触发。
     * R2-02：诊断记账失败时事件绝不允许走向 _Complete——PENDING/完成
     * 路径的 TriggerEvent 配对断言依赖 map 条目，缺失即断言中止。
     * 按「登记未完成」闭环：ARMED→CANCELLED 终态 + 底层事件移交回收
     * （与既有 UnRegist 同一回收路径），返回 -1 使挂起回调以
     * CHECK_FAILED 重入协程——协程侧清 await、未执行的放锁段由调用方
     * -1 分支释放，无 map 残留、无悬挂、无异常穿越 worker。 */
    if (!dbg_regist()) {
        UnRegist();
        return -1;
    }
#endif
    // ARMED 起进入监听态，纳入 fd 唤醒注册表（#262）
    /* #370：登记时若 fd 正在 closing，或自 InitFdEvent 采样 establish-epoch
     * 以来代际已推进（close 完整落在建立窗口内，asio wait 绑在死 fd 上），
     * _TrackWaiter 拒绝登记。此处以 POLL_EVENT_CLOSED 自触使协程恢复后走
     * EBADF，而不是等一个永远不会到达的就绪事件。 */
    if (!_TrackWaiter()) {
        Trigger(POLL_EVENT_CLOSED);
        return 0;   // 已由 CLOSED 路径完成，Regist 语义为"等待建立完成"
    }

    /* 日志装配不分配：Regist() 可经 Context::Resume 的 onyield 回调在
     * worker 原生栈执行，to_string 拼接的 bad_alloc 无协程侧 catch 可达，
     * 出逃即 terminate。栈缓冲 snprintf 保持同一日志语义、消除逃逸点。 */
    char regist_log[192];
    std::snprintf(regist_log, sizeof(regist_log),
                  "[CoEvent:Regist] co=%llu event=%d id=%llu customkey=%d",
                  static_cast<unsigned long long>(m_co_id),
                  static_cast<int>(m_listen_events),
                  static_cast<unsigned long long>(GetId()),
                  m_custom_key);
    g_bbt_dbgp_full(regist_log);
#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_RegistCoPollEvent();
#endif
    return 0;
}

bool CoPollEvent::CommitPark()
{
    /*
     * Context 已切回 Processer，注册回调也已完成；此处才是协程真正提交等待的边界。
     * ARMED 表示尚未触发，发布 PARKED 后触发方可直接完成；PENDING 表示触发已先胜，
     * 必须由当前 Processer 消费其 flags。FINAL/CANCELLED 等终态不再产生回调。
     */
    uint64_t state = m_state.load(std::memory_order_acquire);
    for (;;)
    {
        const auto phase = GetCoPollEventPhase(state);
        if (phase == CoPollEventPhase::ARMED) {
            // 发布 PARKED 是尾操作；成功后 Processer 不得再访问关联 Coroutine。
            if (m_state.compare_exchange_weak(state, PackCoPollEventState(CoPollEventPhase::PARKED),
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire))
                return false;
            continue;
        }

        if (phase != CoPollEventPhase::PENDING)
            return false;

        // 提前触发的 flags 已随状态字发布，只有 CAS 胜者可以执行完成回调。
        const short trigger_events = GetCoPollEventFlags(state);
        if (m_state.compare_exchange_weak(state,
                                          PackCoPollEventState(CoPollEventPhase::TRIGGERING,
                                                               trigger_events),
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire))
        {
            // pending 完成运行在 Processer 线程，不能让 callback 异常逃出。
            try
            {
                _Complete(trigger_events);
            }
            catch (...)
            {
                ReportCoPollEventException(m_co_id, m_event_id, std::current_exception());
            }
            return true;
        }
    }
}

int CoPollEvent::UnRegist()
{
    uint64_t state = m_state.load(std::memory_order_acquire);
    for (;;)
    {
        const auto phase = GetCoPollEventPhase(state);
        if (phase != CoPollEventPhase::INITED &&
            phase != CoPollEventPhase::ARMING &&
            phase != CoPollEventPhase::ARMED &&
            phase != CoPollEventPhase::PARKED)
            return -1;

        if (!m_state.compare_exchange_weak(state, PackCoPollEventState(CoPollEventPhase::CANCELLED),
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire))
            continue;

        // ARMING 的 Regist() 仍可能在 StartListen() 内部，资源清理由它完成。
        if (phase != CoPollEventPhase::ARMING)
            _CannelAllFdEvent();
        return 0;
    }
}

int CoPollEvent::_RegistFdEvent()
{
    return m_event->StartListen((m_timeout < 0 ? 0 : m_timeout));
}

int CoPollEvent::_CannelAllFdEvent()
{
    _UntrackWaiter();
    if (m_event != nullptr)
        g_bbt_poller->DeferDestroyEvent(std::move(m_event));
    return 0;
}

int CoPollEvent::GetEvent() const
{
    return TransformToPollEventType(m_listen_events, m_has_custom_event);
}

bool CoPollEvent::IsListening() const
{
    const auto phase = GetCoPollEventPhase(m_state.load(std::memory_order_acquire));
    return phase == CoPollEventPhase::ARMING ||
        phase == CoPollEventPhase::ARMED ||
        phase == CoPollEventPhase::PARKED;
}

bool CoPollEvent::IsFinal() const
{
    const auto phase = GetCoPollEventPhase(m_state.load(std::memory_order_acquire));
    return phase == CoPollEventPhase::FINAL || phase == CoPollEventPhase::CANCELLED;
}

CoPollEventStatus CoPollEvent::GetStatus() const
{
    switch (GetCoPollEventPhase(m_state.load(std::memory_order_acquire)))
    {
    case CoPollEventPhase::INITED:
        return CoPollEventStatus::POLLEVENT_INITED;
    case CoPollEventPhase::ARMING:
    case CoPollEventPhase::ARMED:
    case CoPollEventPhase::PARKED:
        return CoPollEventStatus::POLLEVENT_LISTEN;
    case CoPollEventPhase::PENDING:
    case CoPollEventPhase::TRIGGERING:
        return CoPollEventStatus::POLLEVENT_TRIGGER;
    case CoPollEventPhase::FINAL:
        return CoPollEventStatus::POLLEVENT_FINAL;
    case CoPollEventPhase::CANCELLED:
        return CoPollEventStatus::POLLEVENT_CANNEL;
    }
    return CoPollEventStatus::POLLEVENT_DEFAULT;
}

CoPollEventId CoPollEvent::GetId() const
{
    return m_event_id;
}

int CoPollEvent::GetFd() const
{
    return m_fd;
}

int64_t CoPollEvent::GetTimeout() const
{
    return m_timeout;
}

int CoPollEvent::_Complete(short trigger_events)
{
    auto keep_alive = shared_from_this();
    _CannelAllFdEvent();
    m_state.store(PackCoPollEventState(CoPollEventPhase::FINAL, trigger_events),
                  std::memory_order_release);

#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_TriggerCoPollEvent();
#endif
#ifdef BBT_COROUTINE_STRINGENT_DEBUG
    g_bbt_dbgmgr->OnEvent_TriggerEvent(keep_alive);
#endif

    const auto callback = m_onevent_callback;
    const int custom_key = m_custom_key;
    std::exception_ptr pending_exception{nullptr};
    try
    {
        callback(keep_alive, trigger_events, custom_key);
    }
    catch(const std::exception& e)
    {
        if (g_bbt_coroutine_config->m_ext_coevent_exception_callback != nullptr)
            g_bbt_coroutine_config->m_ext_coevent_exception_callback(core::errcode::Errcode(e.what()));
        else
            pending_exception = std::current_exception();
    }
    catch(...)
    {
        if (g_bbt_coroutine_config->m_ext_coevent_exception_callback != nullptr)
            g_bbt_coroutine_config->m_ext_coevent_exception_callback(core::errcode::Errcode("unknown exception"));
        else
            pending_exception = std::current_exception();
    }

    if (pending_exception)
        std::rethrow_exception(pending_exception);
    return 0;
}

}
