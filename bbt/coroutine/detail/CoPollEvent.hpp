#pragma once
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <bbt/core/Attribute.hpp>
#include <bbt/pollevent/Event.hpp>
#include <bbt/coroutine/detail/Define.hpp>

namespace bbt::coroutine::detail
{

/**
 * CoroutineEvent：一次等待的生命周期（CAS 无互斥锁）。
 *
 * 注册 → 就绪/超时/自定义唤醒/对端关闭 → 单次完成；或 UnRegist 取消。
 * 重复 Trigger 只有第一次有效。析构走 DeferDestroyEvent，须在 PollOnce 线程回收底层 Event。
 *
 * 内部阶段见 CoPollEventPhase；GetStatus() 是对外粗映射
 * （PENDING→POLLEVENT_TRIGGER，CANCELLED→POLLEVENT_CANNEL，拼写沿用现有公开枚举）。
 * IPollEvent 是未接入遗留接口，本类型不实现它。
 *
 * FINAL / CANCELLED 不可复用。关闭对端 fd 走底层 READABLE/CLOSE，不是独立阶段。
 */
class CoPollEvent:
    public std::enable_shared_from_this<CoPollEvent>
{
public:
    friend class CoPoller;
    typedef std::shared_ptr<CoPollEvent> SPtr;

    static SPtr                     Create(CoroutineId id, const CoPollEventCallback& cb);

    BBTATTR_FUNC_CTOR_HIDDEN        CoPollEvent(CoroutineId id, const CoPollEventCallback& cb);
                                    ~CoPollEvent();

    int                             GetEvent() const;
    bool                            IsListening() const;
    bool                            IsFinal() const;
    CoPollEventStatus               GetStatus() const;
    CoPollEventId                   GetId() const;
    int                             GetFd() const;
    int64_t                         GetTimeout() const;
    /* 登记时采样的 fd 代际（#370）；非 fd 事件时为 0。
     * 该值在 InitFdEvent 建立等待时采样（establish-epoch），_TrackWaiter
     * 与恢复路径都以它作为「本次等待所针对的 fd 代际」基线。 */
    uint64_t                        GetWaitEpoch() const noexcept { return m_wait_epoch; }

    /* 初始化后调用Regist注册事件 */
    int                             InitFdEvent(int fd, short events, int timeout);
    int                             InitCustomEvent(int key, void* args);

    /* 注册、触发、反注册都是线程安全的，意味着可以在任意线程执行 */
    int                             Trigger(short trigger_events);
    int                             Regist();
    int                             UnRegist();
    bool                            CommitPark();

    /**
     * @brief fd 当前代际（#370 epoch 校验）
     *
     * 每次 BeginFdClose 推进 +1；waiter 在登记时锁内采样
     * m_wait_epoch，恢复后与当前值比对即可识别 close+reuse。
     * fd 无记录时返回 0（等价于初始代际）。
     */
    static uint64_t                 FdEpoch(int fd);
    /* 测试探针：fd 上当前登记的活跃 waiter 数（锁内统计未过期项）；
     * 无槽位返回 0。仅用于测试同步，不参与协议判定。 */
    static size_t                   FdWaiterCount(int fd);

    /**
     * @brief close 线性化协议（#370）：close 前调用，锁内 claim+代际推进+摘 waiter
     *
     * WHY: Linux 下 close(fd) 会把 epoll 关注项静默移除，不产生任何事件，
     * 等待该 fd 的协程将永久挂起（#262）。本函数必须在底层 close(fd) 之前
     * 调用：同一临界区内完成代际推进 + closing 置位 + 摘走全部旧 waiter，
     * 保证 close/代际/waiter 摘除在同一线性化点内完成。此后登记的同号 fd
     * waiter 记新一代际、不被本批唤醒误伤。旧 waiter 统一
     * Trigger(POLL_EVENT_CLOSED)；正常事件已就绪但尚未恢复的协程不在本
     * 表内，由恢复后 epoch 比对兜底。
     *
     * 返回 true 表示本调用方获得 close 权（须继续执行底层 close 并随后
     * 调用 EndFdClose）；false 表示该 fd 已被其它线程 claim——此时不得
     * 再对同一数字 fd 发起底层 close（claim 方完成后 fd 号可能已被复用，
     * 第二次 close 会误关新对象），调用方应经 WaitFdCloseReady 等待
     * claim 方收尾后返回 EBADF。
     *
     * 期间到达的同号 fd waiter 会观察到 closing/新 epoch，不被本批误摘。
     */
    static bool                     BeginFdClose(int fd);
    /**
     * @brief BeginFdClose 的收尾：底层 close 已返回后调用，解除 closing 标记。
     * 必须在同一线程、紧接底层 close 之后调用，无论成功或失败。
     */
    static void                     EndFdClose(int fd);
    /**
     * @brief 非 claim 方的有界等待（#370 close 所有权协议）
     *
     * 仅当该 fd 正被其它线程 claim closing 时，以 1ms 粒度自旋等待其
     * EndFdClose（底层 close 已返回），最多 timeout_ms 毫秒，返回 true。
     * 无槽位或未在 closing 立即返回 true；等待超时返回 false。
     *
     * WHY 自旋而非 condition_variable：close 临界区仅含一个 syscall，窗口
     * 为微秒级；本函数只服务于「同一 fd 号并发重复 close」的罕见竞态路径，
     * 不值得为登记表引入 cv 依赖与唤醒开销。
     */
    static bool                     WaitFdCloseReady(int fd, int timeout_ms);
    /* 测试探针：fd 槽位当前是否处于 closing（claim 方底层 close 未返回）。
     * 仅用于测试同步，不参与协议判定。 */
    static bool                     FdIsClosing(int fd);

protected:
    int                             _RegistFdEvent();
    int                             _CannelAllFdEvent();
    int                             _Complete(short trigger_events);
    static CoPollEventId            _GenerateId();
private:
    CoroutineId                     m_co_id{0};
    std::shared_ptr<bbt::pollevent::Event>
                                    m_event{nullptr};
    int                             m_fd{-1};
    short                           m_listen_events{0};
    int                             m_timeout{-1};
    bool                            m_has_custom_event{false};
    int                             m_custom_key{-1};
    CoPollEventId                   m_event_id{BBT_COROUTINE_INVALID_COPOLLEVENT_ID};

    CoPollEventCallback             m_onevent_callback{nullptr};
    std::atomic<uint64_t>           m_state{PackCoPollEventState(CoPollEventPhase::INITED)};

    /* fd → 活跃等待事件注册表 + fd 代际（#262、#370）
     * FdSlot::closing：BeginFdClose 已摘表、底层 close 尚未返回期间的标记；
     * 此窗口内到达的同号 waiter 不登记，由 Regist 尾部以 POLL_EVENT_CLOSED 自触。
     * FdSlot::epoch：建立等待（InitFdEvent）时锁内采样为 m_wait_epoch 基线；
     * 只要 BeginFdClose 推进过（无论 close 是否已返回），登记即拒绝。
     * 槽位持久性：epoch>0 的槽位永不擦除——FdEpoch 对缺席槽位返回 0，
     * 会把它与「从未 close 过」的首代 fd 别名，丢失 epoch 校验能力
     * （#370 round-2 复审发现）。仅 epoch==0 且无 waiter 才摘除。 */
    struct FdSlot
    {
        uint64_t                                epoch{0};
        bool                                    closing{false};
        std::vector<std::weak_ptr<CoPollEvent>> waiters;
    };
    static std::mutex                                       s_waiters_mtx;
    static std::unordered_map<int, FdSlot>                  s_slots;
    /* _TrackWaiter 返回 false 表示登记时 fd 已 closing，或建立等待
     * （InitFdEvent）至今 epoch 已推进——说明底层 asio wait 绑定的 fd 对象
     * 已死亡/换代，不可等待（由 Regist 以 POLL_EVENT_CLOSED 自触）。 */
    bool                            _TrackWaiter();
    void                            _UntrackWaiter();
    /* 建立等待时（InitFdEvent）锁内采样的 fd 代际基线（establish-epoch）；
     * _TrackWaiter 据此判定建立窗口内是否发生过 close，恢复路径据此与
     * FdEpoch 比对识别挂起期间的 close+reuse。 */
    uint64_t                        m_wait_epoch{0};
};

}
