/* 单个 fd/定时器事件 Event（asio 封装）。
 *
 * 线程模型：StartListen/CancelListen/Trigger 与 asio 完成回调可由不同线程并发
 * 进入，全部成员与底层 asio 对象由 m_mutex 串行化；用户回调一律在锁外调用与
 * 销毁，避免在持锁期间重入。
 *
 * 寿命：异步等待只捕获 weak_ptr，回调执行期间用 weak.lock() 的局部强引用保活；
 * Event 析构后排队中的回调自然丢弃。因此异步登记（StartListen/Trigger）要求
 * Event 已由 shared_ptr 持有，栈对象调用返回 -1；构造、CancelListen 与析构
 * 不受此限制。
 *
 * 取消：CancelListen 只撤销本轮等待并归还对象，不调用 restart()/poll() 驱动
 * io_context；已排队的完成由下一次驱动派发，按轮次与 weak 句柄丢弃。析构走同一
 * 路径，只使用不抛异常的 Asio 操作。
 *
 * 交付边界：完成回调在持锁时校验轮次，用户回调在锁外调用；已通过校验的那一次
 * 交付若与另一线程的 StartListen/CancelListen 并发，仍可能在新一轮开始或取消
 * 返回后到达一次，但 PERSIST 重挂前会再次校验轮次，不会复活旧监听。
 */
#pragma once
#include <bbt/pollevent/detail/Define.hpp>
#include <bbt/pollevent/detail/EventBase.hpp>
#include <boost/asio.hpp>
#include <cstdint>
#include <memory>
#include <mutex>

namespace bbt::pollevent {

class Event : public std::enable_shared_from_this<Event> {
public:
    typedef std::shared_ptr<Event> SPtr;

    Event(detail::EventBase* base, evutil_socket_t fd, short listen_events, const OnEventCallback& onevent_cb);
    ~Event();

    int StartListen(uint64_t timeout);
    int CancelListen(bool need_close_fd = false);
    int GetSocket() const;
    short GetEvents() const;
    EventId GetEventId();
    int64_t GetTimeoutMs() const;

    /* 自定义事件触发；连续成功调用分别排队交付，StartListen/CancelListen 可淘汰旧 post。 */
    int Trigger(int flag);

    /* 测试探针：callback_map 当前条目数（构造失败回滚验证用，不参与协议）。 */
    static size_t CallbackEntryCount();

#ifdef BBT_POLLEVENT_TESTING
    /* 测试注入点（仅 NEED_TEST 构建）：1 = StartListen 临界区内、fd 等待已挂且
     * timer 未挂；2 = fd 完成回调进入、尝试取 m_mutex 之前。钩子在持锁（1）或
     * 取锁前（2）执行，只能做线程同步，不得重入本对象；须在启动相关线程前设置。 */
    enum class TestHookPoint { StartListenArmedFd = 1, FdCompletionEnter = 2 };
    static void SetTestHook(void (*hook)(Event* self, TestHookPoint point));
#endif

private:
    EventId GenerateId();

    friend class EventLoop;
    static void AddEventCallback(EventId id, const OnEventCallback& cb);
    /* 取出并移除注册项：用户回调对象的析构留给调用方在锁外完成——捕获对象
     * 可能持有本 Event 自身（_EventHelper 即如此），在注册表锁内析构会重入。 */
    static OnEventCallback TakeEventCallback(EventId id);
    static void CallEventCallback(EventId id, int fd, short events);

    /* 以下 *Locked 系列要求调用方已持有 m_mutex。 */
    void DoAsyncWaitLocked(short event_flag, uint64_t round);
    void ArmTimerLocked(uint64_t timeout_ms, uint64_t round);
    void CancelPendingLocked();
    /* 归还 asio 对象与内部 dup fd；取消与析构路径共用，保证同一 fd 只关一次。 */
    void ReleaseObjectsLocked();

    /* 完成回调入口：round 是挂载时的轮次号，与当前轮次不符即丢弃。 */
    void OnFdWaitCompleted(uint64_t round, short event_flag, int fd);
    void OnTimerFired(uint64_t round);
    void OnCustomTriggered(uint64_t round, int fd, short flag);

    /* 首次异步登记时绑定自身 weak 句柄；未由 shared_ptr 持有时返回 false。 */
    bool EnsureWeakSelfLocked();

#ifdef BBT_POLLEVENT_TESTING
    static void RunTestHook(Event* self, TestHookPoint point);
#endif

private:
    EventId m_id{0};
    detail::EventBase* m_ref_base{nullptr};
    /* 用户 fd 号；need_close_fd 关闭后置 -1，保证重复调用不会二次关闭。 */
    evutil_socket_t m_fd{-1};
    /* fd 事件实际交给 asio 的内部 dup fd（构造函数 ::dup(m_fd)），
     * 与用户 fd 号生命周期解耦。构造成功后由 m_sd adopt 持有，析构/取消
     * 走 release()+::close 单一收口；构造中失败时同口径只关一次。
     * 非 fd 事件为 -1。 */
    evutil_socket_t m_internal_fd{-1};
    short m_listen_events{0};
    int64_t m_timeout{-1};

    /* 串行化 StartListen/CancelListen/Trigger 与完成回调的底层操作。 */
    mutable std::mutex m_mutex;
    /* 监听轮次：仅 StartListen/CancelListen 推进；完成回调携带挂载时轮次号，旧轮次
     * 结果不得污染新监听。Trigger 读取当前轮次，不使其它已排队 Trigger 失效。 */
    uint64_t m_round{0};
    /* 完成回调保活用：只持 weak，不延长 Event 寿命。 */
    std::weak_ptr<Event> m_self;

    /* ASIO 对象 — 二选一 */
    std::unique_ptr<boost::asio::posix::stream_descriptor> m_sd;
    std::unique_ptr<boost::asio::steady_timer> m_timer;

    struct CallbackRegistry;
    static CallbackRegistry& _Registry();
};

} // namespace bbt::pollevent
