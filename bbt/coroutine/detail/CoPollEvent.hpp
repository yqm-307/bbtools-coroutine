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

    /* 初始化后调用Regist注册事件 */
    int                             InitFdEvent(int fd, short events, int timeout);
    int                             InitCustomEvent(int key, void* args);

    /* 注册、触发、反注册都是线程安全的，意味着可以在任意线程执行 */
    int                             Trigger(short trigger_events);
    int                             Regist();
    int                             UnRegist();
    bool                            CommitPark();

    /**
     * @brief 唤醒正在等待 fd 的全部协程事件（#262）
     *
     * WHY: Linux 下 close(fd) 会把 epoll 关注项静默移除，不产生任何事件，
     * 等待该 fd 的协程将永久挂起。Hook_Close 在真正 close 前调用本函数，
     * 让被唤醒的协程重试 syscall 拿到 EBADF，按原生语义返回 -1/EBADF。
     */
    static void                     WakeupFdWaiters(int fd);

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

    /* fd → 活跃等待事件注册表（WakeupFdWaiters 用，#262） */
    static std::mutex                                       s_waiters_mtx;
    static std::unordered_map<int, std::vector<std::weak_ptr<CoPollEvent>>> s_waiters;
    void                            _TrackWaiter();
    void                            _UntrackWaiter();
};

}
