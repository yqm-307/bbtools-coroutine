#include <atomic>
#include <cerrno>
#include <cstdio>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <boost/asio.hpp>
#include <bbt/pollevent/Event.hpp>
#include <bbt/pollevent/detail/EventBase.hpp>
#include <bbt/core/clock/Clock.hpp>

namespace bbt::pollevent
{

struct Event::CallbackRegistry
{
    std::unordered_map<EventId, OnEventCallback> m_callbacks;
    std::mutex m_mutex;
};

Event::CallbackRegistry& Event::_Registry()
{
    // 与 runtime 保持进程寿命，避免静态退出与 worker 删除回调竞争。
    // Event 取消/析构仍逐项释放回调，仅注册表及其锁不做静态析构。
    static auto* holder = new CallbackRegistry();
    return *holder;
}

#ifdef BBT_POLLEVENT_TESTING
namespace
{
/* 测试串行设置：测试在启动装配/完成线程之前写入，线程 join 之后才清除。 */
void (*g_test_hook)(Event*, Event::TestHookPoint) = nullptr;
} // namespace

void Event::SetTestHook(void (*hook)(Event*, TestHookPoint))
{
    g_test_hook = hook;
}

void Event::RunTestHook(Event* self, TestHookPoint point)
{
    if (g_test_hook != nullptr)
        g_test_hook(self, point);
}
#endif

Event::Event(detail::EventBase* base, evutil_socket_t fd, short listen_events,
             const OnEventCallback& onevent_cb)
    :m_id(GenerateId()),
     m_ref_base(base),
     m_fd(fd),
     m_listen_events(listen_events)
{
    Assert(base != nullptr);

    /* 先完成全部可能失败的资源创建，再注册 callback——构造
     * 失败时既无 callback_map 残留条目，也无内部 dup fd 泄漏，无需逐个
     * 异常出口回滚（此前 AddEventCallback 在 dup 之前，dup/timer 抛异常会
     * 留下孤儿回调条目）。 */
    // 判断事件类型并创建 ASIO 对象（fd 和 timer 可共存）
    bool is_fd_event = (fd >= 0) && (listen_events & (EV_READ | EV_WRITE | EV_CLOSED));
    bool is_timer   = (listen_events & EV_TIMEOUT);

    if (is_fd_event) {
        Assert(fd >= 0);
        /* asio stream_descriptor 是 adopt 语义（不 dup，直接持有 fd
         * 号），epoll_reactor 内部以 fd 号为 key 管理注册项。用户侧
         * close+dup2 复用同一 fd 号后，本 Event 的延迟析构（DeferDestroy）
         * 会按 fd 号 deregister——误删同号新 Event 刚注册的 epoll 项，
         * 造成新 waiter 永久挂起。
         * 根治：内部 dup 一个私有 fd 号供 asio 全程使用。旧 Event 析构时
         * deregister/close 的都是内部号，与用户 fd 号生命周期解耦；
         * 用户 fd 的 close/reuse 不再影响底层注册的准确性。 */
        m_internal_fd = ::dup(fd);
        /* dup 失败（EMFILE/ENFILE 资源枯竭）不能静默 adopt -1——asio 对
         * 负 fd 的行为未定义。抛异常交由 InitFdEvent 的既有重试/失败路径
         * 收敛为「等待建立失败 -1」，协程存活。 */
        if (m_internal_fd < 0)
            throw std::system_error(errno, std::generic_category(),
                                    "pollevent dup fd failed");
        try {
            m_sd = std::make_unique<boost::asio::posix::stream_descriptor>(
                base->GetContext(), m_internal_fd);
        }
        catch (...) {
            ::close(m_internal_fd);
            m_internal_fd = -1;
            throw;
        }
    }
    if (is_timer) {
        try {
            m_timer = std::make_unique<boost::asio::steady_timer>(base->GetContext());
        }
        catch (...) {
            /* fd 事件先于 timer 创建：timer 构造抛异常时回收内部 dup fd。
             * m_sd 自构造起即持有 m_internal_fd（adopt 语义），destroy 会对
             * 仍 open 的 descriptor 再 close 一次——先 release() 剥离所有权
             * 再 ::close，保证同一 fd 号只关一次，与析构口径一致
             * （此时 callback 尚未注册，fd 是唯一待回收资源）。 */
            if (m_sd) {
                m_sd->release();
            }
            if (m_internal_fd >= 0) {
                ::close(m_internal_fd);
                m_internal_fd = -1;
            }
            m_sd.reset();
            throw;
        }
    }

    /* 全部资源就绪后才注册回调并计数——此后构造不再失败，析构走
     * CancelListen 正常回收（DelEventCallback + close 内部 fd）。 */
    AddEventCallback(m_id, onevent_cb);
    m_ref_base->IncEventCount();
}

Event::~Event()
{
    /* 取消等待、摘除用户回调（可能在锁外重入本对象），并归还 asio 对象与内部
     * dup fd。析构后排队中的完成回调只能拿到失效的 weak_ptr。 */
    CancelListen();
    m_ref_base->DecEventCount();
}

bool Event::EnsureWeakSelfLocked()
{
    if (m_self.expired())
        m_self = weak_from_this();
    return !m_self.expired();
}

void Event::CancelPendingLocked()
{
    if (m_sd) {
        boost::system::error_code ec;
        m_sd->cancel(ec);
    }
    if (m_timer) {
        /* Boost 1.90（Asio 1.38）的 basic_waitable_timer 只有抛异常的 cancel()，
         * 没有 error_code 重载；这里只服务 StartListen/Trigger 这类非析构路径。
         * 析构路径改走 ReleaseObjectsLocked 的 timer 销毁（内部用 error_code
         * cancel），见该函数注释。 */
        m_timer->cancel();
    }
}

int Event::StartListen(uint64_t timeout)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    /* 异步登记只在 shared ownership 建立后成立：没有强引用就没有办法在回调
     * 执行期间保活。栈上构造仍可用于构造/取消/析构，但不能进入监听。 */
    if (!EnsureWeakSelfLocked()) {
        static std::atomic_bool reported{false};
        if (!reported.exchange(true))
            std::fprintf(stderr,
                         "[pollevent] StartListen requires shared_ptr ownership of Event id=%lld\n",
                         static_cast<long long>(m_id));
        return -1;
    }

    /* 新轮次：上一轮已排队的完成回调与 PERSIST 重挂全部作废。 */
    const uint64_t round = ++m_round;

    if (timeout > 0) {
        m_timeout = bbt::core::clock::nowAfter(
            bbt::core::clock::milliseconds(timeout + 1))
            .time_since_epoch().count();
    } else {
        m_timeout = -1;
    }

    /* 撤销上一轮的底层等待（fd 与 timer 可共存）：此后只有本轮挂载可以交付。 */
    CancelPendingLocked();

    // fd 事件（可与 timer 共存）
    if (m_sd) {
        if (m_listen_events & EV_READ)
            DoAsyncWaitLocked(EV_READ, round);
        if (m_listen_events & EV_WRITE)
            DoAsyncWaitLocked(EV_WRITE, round);
    }

#ifdef BBT_POLLEVENT_TESTING
    /* 装配中点：fd 等待已挂、timer 尚未挂；测试在此证明完成回调与装配竞争。 */
    RunTestHook(this, TestHookPoint::StartListenArmedFd);
#endif

    // 定时器事件（可与 fd 事件共存）
    if (m_timer) {
        if (timeout > 0)
            ArmTimerLocked(timeout, round);
        return 0;
    }

    // 自定义事件：无 fd 无 timer，由 Trigger() 驱动；仅超时需要在此时建 timer
    if (timeout > 0 && m_fd < 0) {
        m_timer = std::make_unique<boost::asio::steady_timer>(
            m_ref_base->GetContext());
        ArmTimerLocked(timeout, round);
    }

    return 0;
}

void Event::ArmTimerLocked(uint64_t timeout_ms, uint64_t round)
{
    m_timer->expires_after(std::chrono::milliseconds(timeout_ms));

    std::weak_ptr<Event> weak_self = m_self;
    m_timer->async_wait([weak_self, round](boost::system::error_code ec) {
        if (ec == boost::asio::error::operation_aborted)
            return;
        if (auto self = weak_self.lock())
            self->OnTimerFired(round);
    });
}

void Event::DoAsyncWaitLocked(short event_flag, uint64_t round)
{
    if (!m_sd)
        return;

    auto wait_type = (event_flag == EV_READ)
        ? boost::asio::posix::stream_descriptor::wait_read
        : boost::asio::posix::stream_descriptor::wait_write;

    const int fd = m_fd;
    std::weak_ptr<Event> weak_self = m_self;

    m_sd->async_wait(wait_type,
        [weak_self, round, event_flag, fd](boost::system::error_code ec) {
            if (ec == boost::asio::error::operation_aborted)
                return;
            if (auto self = weak_self.lock())
                self->OnFdWaitCompleted(round, event_flag, fd);
        });
}

void Event::OnFdWaitCompleted(uint64_t round, short event_flag, int fd)
{
#ifdef BBT_POLLEVENT_TESTING
    /* 入口点：尚未取 m_mutex，测试可在此确认完成回调已进入、正与装配竞争。 */
    RunTestHook(this, TestHookPoint::FdCompletionEnter);
#endif
    bool persist = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        /* 取消/重启/自定义触发之后到达的旧轮次成功回调不得交付，也不得重挂。 */
        if (round != m_round || !m_sd)
            return;
        if (m_timer)
            m_timer->cancel();      // fd 已交付：撤销定时器，防止 double-fire
        /* m_listen_events 构造后只读；仍在锁内取快照，回调路径不直接读成员。 */
        persist = (m_listen_events & EV_PERSIST) != 0;
    }

    /* 锁外调用：用户回调可能重入 StartListen/CancelListen/析构本对象。 */
    CallEventCallback(m_id, fd, event_flag);

    if (!persist)
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    /* 回调期间若已取消或重启，新一轮的挂载由那一侧负责，这里不得重复挂。 */
    if (round != m_round)
        return;
    DoAsyncWaitLocked(event_flag, round);
}

void Event::OnTimerFired(uint64_t round)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (round != m_round || !m_timer)
            return;
        if (m_sd) {
            boost::system::error_code ignored;
            m_sd->cancel(ignored);  // timer 已交付：撤销 fd 等待，防止 double-fire
        }
    }

    CallEventCallback(m_id, -1, EV_TIMEOUT);
}

int Event::CancelListen(bool need_close_fd)
{
    int user_fd = -1;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_round;              // 之后到达的完成回调一律作废
        m_timeout = -1;
        if (need_close_fd && m_fd >= 0) {
            /* need_close_fd 语义：由调用方要求连用户 fd 一起关（保留旧行为）。
             * 取出后置 -1，重复取消不会二次关闭同一 fd。 */
            user_fd = m_fd;
            m_fd = -1;
        }
        /* 取消后本对象不再交付（用户回调随后摘除），asio 对象与内部 dup fd
         * 在此一次性归还：release() 与 timer 销毁都会把挂起操作以
         * operation_aborted 投递，且都不抛异常——析构（经本函数）安全。 */
        ReleaseObjectsLocked();
    }

    if (user_fd >= 0)
        ::close(user_fd);

    /* 不在此驱动 io_context：restart/poll 排空会把取消方变成第二个驱动者，
     * 并与正在运行 poll 的线程争用同一 io_context。到期且已排队的完成回调
     * 由轮次校验丢弃。
     * 摘除的回调在锁外析构：其捕获对象可能持有本 Event（_EventHelper 即如此），
     * 在注册表锁或 m_mutex 内析构会重入 CancelListen。 */
    OnEventCallback removed = TakeEventCallback(m_id);

    return 0;
}

int Event::Trigger(int flag)
{
    std::weak_ptr<Event> weak_self;
    uint64_t round = 0;
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!EnsureWeakSelfLocked()) {
            static std::atomic_bool reported{false};
            if (!reported.exchange(true))
                std::fprintf(stderr,
                             "[pollevent] Trigger requires shared_ptr ownership of Event id=%lld\n",
                             static_cast<long long>(m_id));
            return -1;
        }
        /* 自定义触发会撤销底层等待，但不推进监听轮次：每次成功 post 都应交付，
         * 只有 StartListen/CancelListen 才会使已排队触发失效。 */
        round = m_round;
        weak_self = m_self;
        /* fd 必须在本临界区内快照：CancelListen(true) 会在锁内把 m_fd 置 -1，
         * post 出去的回调只读这份快照，不再回头读可变成员。 */
        fd = m_fd;
        CancelPendingLocked();
    }

    const auto events = static_cast<short>(flag);
    boost::asio::post(m_ref_base->GetContext(),
        [weak_self, round, fd, events]() {
            if (auto self = weak_self.lock())
                self->OnCustomTriggered(round, fd, events);
        });
    return 0;
}

void Event::OnCustomTriggered(uint64_t round, int fd, short flag)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (round != m_round)
            return;
    }
    CallEventCallback(m_id, fd, flag);
}

void Event::ReleaseObjectsLocked()
{
    if (m_sd) {
        /* Asio 1.38 的 basic_descriptor::release() 没有 error_code 重载，但实现
         * 只有 deregister_descriptor（epoll_ctl DEL 结果忽略）、cleanup 与状态
         * 复位，没有返回错误的路径；挂起操作被置 operation_aborted 并投递。
         * 返回值是内部 dup fd 号，由下一行 ::close 单次收口。 */
        (void)m_sd->release();
        m_sd.reset();
    }
    if (m_internal_fd >= 0) {
        ::close(m_internal_fd);
        m_internal_fd = -1;
    }
    if (m_timer) {
        /* 不调用 cancel()：1.90 的 basic_waitable_timer 只有抛异常的 cancel()；
         * 销毁 timer 会经 deadline_timer_service::destroy 用 error_code cancel
         * 等价撤销挂起等待且不抛——析构路径因此异常安全。 */
        m_timer.reset();
    }
}

int Event::GetSocket() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_fd;
}

short Event::GetEvents() const
{
    return m_listen_events;
}

EventId Event::GetEventId()
{
    return m_id;
}

int64_t Event::GetTimeoutMs() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_timeout;
}

EventId Event::GenerateId()
{
    static std::atomic_uint64_t _id{0};
    return (++_id);
}

void Event::AddEventCallback(EventId id, const OnEventCallback& cb)
{
    auto& reg = _Registry();
    std::lock_guard<std::mutex> lock(reg.m_mutex);
    reg.m_callbacks[id] = cb;
}

OnEventCallback Event::TakeEventCallback(EventId id)
{
    OnEventCallback removed;
    auto& reg = _Registry();
    std::lock_guard<std::mutex> lock(reg.m_mutex);
    auto it = reg.m_callbacks.find(id);
    if (it == reg.m_callbacks.end())
        return removed;
    removed = std::move(it->second);
    reg.m_callbacks.erase(it);
    return removed;
}

size_t Event::CallbackEntryCount()
{
    auto& reg = _Registry();
    std::lock_guard<std::mutex> lock(reg.m_mutex);
    return reg.m_callbacks.size();
}

void Event::CallEventCallback(EventId id, int fd, short events)
{
    OnEventCallback cb = nullptr;
    {
        auto& reg = _Registry();
        std::lock_guard<std::mutex> lock(reg.m_mutex);
        auto it = reg.m_callbacks.find(id);
        if (it != reg.m_callbacks.end())
            cb = it->second;
    }
    if (cb) cb(fd, events, id);
}

} // namespace bbt::pollevent
