/**
 * @file __TChan.hpp
 * @brief Chan 的模板实现，由 Chan.hpp 末尾包含。
 */
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <list>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/core/util/Assert.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/DriverWakeDeferral.hpp>
#include <bbt/coroutine/detail/WaitNode.hpp>

namespace bbt::coroutine::sync {

/**
 * @name stream operators
 * @{
 */

/**
 * @brief 转发 Chan::Write 并折算成 bool：成功（0）为 true，可能挂起。
 */
template <class TItem, int Max> bool operator<<(Chan<TItem, Max>& chan, const typename Chan<TItem, Max>::ItemType& item)
{
    return (chan.Write(item) == 0);
}

/**
 * @brief 转发 Chan::Read 并折算成 bool：成功（0）为 true，可能挂起。
 */
template <class TItem, int Max> bool operator>>(Chan<TItem, Max>& chan, typename Chan<TItem, Max>::ItemType& item)
{
    return (chan.Read(item) == 0);
}

/**
 * @brief 经 shared_ptr 转发 Write，成功（0）为 true。
 */
template <class TItem, int Max> bool operator<<(std::shared_ptr<Chan<TItem, Max>> chan, const TItem& item)
{
    return (chan->Write(item) == 0);
}

/**
 * @brief 经 shared_ptr 转发 Read，成功（0）为 true。
 */
template <class TItem, int Max> bool operator>>(std::shared_ptr<Chan<TItem, Max>> chan, TItem& item)
{
    return (chan->Read(item) == 0);
}

/**
 * @}
 */

template <class TItem, int Max> class ChannelCore : public std::enable_shared_from_this<ChannelCore<TItem, Max>> {
    static_assert(Max >= 0, "Channel capacity must be non-negative");
    enum class State { Pending, Matched, Completed, Failed, Closed, Cancelled, TimedOut, Unavailable };
    enum class Attempt { Done, Register, Park };
    using Clock = std::chrono::steady_clock;
    using Value = std::unique_ptr<TItem>;
    using Batch = std::vector<Value>;

    struct EmptyBuffer {};

    using Buffer = std::conditional_t<(Max > 0), std::array<Value, Max>, EmptyBuffer>;
    using Watchers = std::vector<CoWaiter::SPtr>;
    using Wait = std::shared_ptr<detail::WaitNode>;
    struct Operation;
    using Op = std::shared_ptr<Operation>;
    using Queue = std::list<Op>;

    struct Operation {
        Operation(
            bool sending, Value& value, Clock::time_point deadline, Batch* batch = nullptr, bool drain_on_close = false)
            : sending(sending)
            , value(value)
            , deadline(deadline)
            , batch(batch)
            , drain_on_close(drain_on_close)
        {
        }

        const bool sending;
        /**
         * 等待方保活栈槽，只有持锁且操作仍 Pending 时才能写入或取得槽中所有权。
         */
        Value& value;
        const Clock::time_point deadline;
        Batch* batch;
        const bool drain_on_close;
        State state{State::Pending};
        std::optional<typename Queue::iterator> position;
        Wait wait;
        Op sender;
    };

public:
    enum class Mode { Immediate, Blocking, Timed };

    int Send(const TItem& item, Mode mode, int timeout = 0)
    {
        auto* co = CurrentCoroutine();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_closed)
                return Result(State::Closed);
            if (co && co->IsCancelRequested())
                return Result(State::Cancelled);
            if (mode == Mode::Immediate && m_receivers.empty() && m_size == Max)
                return Result(State::Unavailable);
        }

        const auto deadline = Deadline(mode, timeout);
        auto value = std::make_unique<TItem>(item);
        auto op = std::make_shared<Operation>(true, value, deadline);
        Start(op, co, mode);
        std::lock_guard<std::mutex> lock(m_mutex);
        /**
         * 零缓冲的非阻塞写只承诺配对，不等待接收端赋值。
         */
        if (mode == Mode::Immediate && (op->state == State::Matched || op->state == State::Failed))
            return 0;
        return Result(op->state);
    }

    template <class Deliver> int Receive(Deliver deliver, Mode mode, int timeout = 0, bool all = false)
    {
        Value value;
        Batch batch;
        if constexpr (Max > 1) {
            if (all)
                batch.reserve(Max - 1);
        }
        auto op = std::make_shared<Operation>(
            false, value, Deadline(mode, timeout), all ? &batch : nullptr, mode == Mode::Blocking);
        Start(op, CurrentCoroutine(), mode);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (op->state != State::Matched)
                return Result(op->state);
        }
        if constexpr (Max > 1) {
            if (all)
                CompleteBatch(op);
        }
        try {
            deliver(std::as_const(*value));
            for (const auto& entry : batch)
                deliver(std::as_const(*entry));
        } catch (...) {
            /**
             * 配对后容量可能已被下一位写者占用；已保留的值不能回队。
             */
            Finish(op, State::Failed);
            throw;
        }
        Finish(op, State::Completed);
        return Result(State::Completed);
    }

    void Close()
    {
        Queue wake;
        Queue matched;
        {
            detail::DriverWakeDeferral defer;
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_closed)
                return;
            m_closed = true;
            wake.splice(wake.end(), m_senders);
            if constexpr (Max > 0) {
                /**
                 * 正常配对时等待接收者与缓冲不会并存；关闭仍按同一排空规则处理，
                 * 且已拒绝的写者不能在排空过程中补位。
                 */
                while (m_size != 0 && !m_receivers.empty()) {
                    const auto& receiver = m_receivers.front();
                    if (receiver->drain_on_close) {
                        ReserveBuffered(receiver);
                        receiver->position.reset();
                        matched.splice(matched.end(), m_receivers, m_receivers.begin());
                    } else {
                        wake.splice(wake.end(), m_receivers, m_receivers.begin());
                    }
                }
            }
            wake.splice(wake.end(), m_receivers);
            for (const auto& op : wake) {
                op->state = State::Closed;
                op->position.reset();
            }
            NotifyWatchers(m_readWatchers);
            NotifyWatchers(m_writeWatchers);
        }
        for (const auto& op : matched)
            Signal(op, detail::POLL_EVENT_CUSTOM);
        for (const auto& op : wake)
            Signal(op, detail::POLL_EVENT_CLOSED);
    }

    bool IsClosed()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_closed;
    }

    size_t Size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_size;
    }

    void AddWatcher(bool writing, const CoWaiter::SPtr& watcher)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        (writing ? m_writeWatchers : m_readWatchers).push_back(watcher);
    }

    void RemoveWatcher(bool writing, const CoWaiter::SPtr& watcher)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& watchers = writing ? m_writeWatchers : m_readWatchers;
        watchers.erase(std::remove(watchers.begin(), watchers.end(), watcher), watchers.end());
    }

private:
    static Clock::time_point Deadline(Mode mode, int timeout)
    {
        return mode == Mode::Timed && timeout > 0 ? Clock::now() + std::chrono::milliseconds(timeout)
                                                  : Clock::time_point::max();
    }

    static detail::Coroutine* CurrentCoroutine()
    {
        return g_bbt_tls_helper->EnableUseCo() ? g_bbt_tls_coroutine_co : nullptr;
    }

    static int Result(State state)
    {
        switch (state) {
        case State::Completed:
            return 0;
        case State::Closed:
            return -1;
        case State::TimedOut:
            return 1;
        default:
            return -2;
        }
    }

    static void Signal(const Op& op, short reason)
    {
        if (op->wait)
            op->wait->Trigger(reason);
    }

    void Start(const Op& op, detail::Coroutine* co, Mode mode)
    {
        if (Admit(op, co, mode, {}) != Attempt::Register)
            return;

        if (!co
            || !co->_WaitForOperation(
                op->deadline,
                [core = this->weak_from_this(), weak_op = std::weak_ptr<Operation>(op)](short events) {
                    auto owner = core.lock();
                    auto operation = weak_op.lock();
                    return owner && operation
                           && owner->Resolve(
                               operation, events == detail::POLL_EVENT_TIMEOUT ? State::TimedOut : State::Cancelled);
                },
                [&](const Wait& wait) { return Admit(op, co, mode, wait) == Attempt::Park; })) {
            Resolve(op, State::Unavailable);
        }
    }

    Attempt Admit(const Op& op, detail::Coroutine* co, Mode mode, const Wait& wait)
    {
        Queue expired;
        Queue pending;
        Queue admitted;
        if (wait)
            pending.push_back(op);
        Op reader_to_wake;
        Attempt next;
        {
            detail::DriverWakeDeferral defer;
            std::lock_guard<std::mutex> lock(m_mutex);
            next = AdmitLocked(op, co, mode, wait, pending, expired, admitted, reader_to_wake);
        }
        SignalSends(expired, admitted);
        if (reader_to_wake)
            Signal(reader_to_wake, detail::POLL_EVENT_CUSTOM);
        return next;
    }

    Attempt AdmitLocked(
        const Op& op,
        detail::Coroutine* co,
        Mode mode,
        const Wait& wait,
        Queue& pending,
        Queue& expired,
        Queue& admitted,
        Op& reader_to_wake)
    {
        if (op->state != State::Pending)
            return Attempt::Done;
        /**
         * 缓冲残留只允许 Read / ReadAll 排空；两种 TryRead 都将关闭视为终态。
         */
        if (m_closed && (!op->drain_on_close || m_size == 0)) {
            op->state = State::Closed;
            return Attempt::Done;
        }
        if (co && co->IsCancelRequested()) {
            op->state = State::Cancelled;
            return Attempt::Done;
        }
        const auto now = Clock::now();
        if (op->deadline <= now) {
            op->state = State::TimedOut;
            return Attempt::Done;
        }

        if constexpr (Max > 0) {
            if (!op->sending && m_size != 0) {
                ReserveBuffered(op);

                /**
                 * 先为本次读（含 ReadAll 快照）保留旧值，再按 FIFO 给等待写者补位。
                 * 补位仍在同一锁内，避免后来的 TryWrite 抢走刚释放的容量。
                 */
                OnBufferSpace(now, expired, admitted);
                return Attempt::Done;
            }
        }

        auto& peers = op->sending ? m_receivers : m_senders;
        ExpireFront(peers, now, expired);
        const bool await_delivery = Max == 0 && op->sending && mode != Mode::Immediate;
        if (!peers.empty()) {
            if (await_delivery && !wait)
                return Attempt::Register;
            auto peer = std::move(peers.front());
            peers.pop_front();
            peer->position.reset();
            const auto& sender = op->sending ? op : peer;
            const auto& receiver = op->sending ? peer : op;
            receiver->state = State::Matched;
            receiver->value = std::move(sender->value);

            /**
             * 有缓冲写入在接纳值时完成，即使等待读者直接取得该值。
             * 只有零缓冲交接的阻塞写者等待接收端赋值结果。
             */
            if constexpr (Max == 0) {
                op->wait = wait;
                sender->state = State::Matched;
                receiver->sender = sender;
            } else {
                sender->state = State::Completed;
            }
            if (op->sending)
                reader_to_wake = std::move(peer);
            return await_delivery ? Attempt::Park : Attempt::Done;
        }

        if constexpr (Max > 0) {
            if (op->sending && m_size < Max) {
                PushBuffered(std::move(op->value));
                op->state = State::Completed;
                NotifyWatchers(m_readWatchers);
                return Attempt::Done;
            }
        }

        if (mode == Mode::Immediate || (mode == Mode::Timed && op->deadline == Clock::time_point::max())) {
            op->state = mode == Mode::Immediate ? State::Unavailable : State::TimedOut;
            return Attempt::Done;
        }
        if (!wait)
            return Attempt::Register;
        auto& queue = op->sending ? m_senders : m_receivers;
        op->position = pending.begin();
        queue.splice(queue.end(), pending);
        op->wait = wait;
        return Attempt::Park;
    }

    static void ExpireFront(Queue& queue, Clock::time_point now, Queue& expired)
    {
        /**
         * 定时器投递延迟不延长已入队操作的匹配期限。
         */
        while (!queue.empty() && queue.front()->deadline <= now) {
            const auto& op = queue.front();
            op->state = State::TimedOut;
            op->position.reset();
            expired.splice(expired.end(), queue, queue.begin());
        }
    }

    void PushBuffered(Value value)
    {
        m_buffer[(m_head + m_size) % Max] = std::move(value);
        ++m_size;
    }

    Value PopBuffered()
    {
        auto value = std::move(m_buffer[m_head]);
        m_head = (m_head + 1) % Max;
        --m_size;
        return value;
    }

    void ReserveBuffered(const Op& receiver)
    {
        receiver->value = PopBuffered();
        ReserveBatchTail(receiver, Max - 1);
        receiver->state = State::Matched;
    }

    void ReserveBatchTail(const Op& receiver, size_t limit)
    {
        if (auto* batch = std::exchange(receiver->batch, nullptr)) {
            while (m_size != 0 && batch->size() < limit)
                batch->push_back(PopBuffered());
        }
    }

    void CompleteBatch(const Op& receiver)
    {
        Queue expired;
        Queue admitted;
        {
            detail::DriverWakeDeferral defer;
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!receiver->batch || m_size == 0)
                return;

            /**
             * 空等的 ReadAll 已取得直接交接的首值，恢复时再取当下的缓冲快照。
             * 快照包括首值在内最多 Max 个元素，不把本轮补位纳入快照。
             */
            ReserveBatchTail(receiver, Max - 1);
            OnBufferSpace(Clock::now(), expired, admitted);
        }
        SignalSends(expired, admitted);
    }

    void OnBufferSpace(Clock::time_point now, Queue& expired, Queue& admitted)
    {
        if (m_closed)
            return;
        while (m_size < Max) {
            ExpireFront(m_senders, now, expired);
            if (m_senders.empty())
                break;
            const auto& sender = m_senders.front();
            PushBuffered(std::move(sender->value));
            sender->state = State::Completed;
            sender->position.reset();
            admitted.splice(admitted.end(), m_senders, m_senders.begin());
        }
        if (!admitted.empty())
            NotifyWatchers(m_readWatchers);
        if (m_size < Max)
            NotifyWatchers(m_writeWatchers);
    }

    static void SignalSends(const Queue& expired, const Queue& admitted)
    {
        for (const auto& op : expired)
            Signal(op, detail::POLL_EVENT_TIMEOUT);
        for (const auto& op : admitted)
            Signal(op, detail::POLL_EVENT_CUSTOM);
    }

    /**
     * 通知与就绪变化一起锁存，避免在 watcher 的下一轮等待上重放旧通知。
     * 调用方的 DriverWakeDeferral 确保已挂起 waiter 的完成在解锁后投递。
     */
    static void NotifyWatchers(const Watchers& watchers)
    {
        for (const auto& watcher : watchers)
            watcher->Notify();
    }

    bool Resolve(const Op& op, State state)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (op->state != State::Pending)
            return false;
        op->state = state;
        if (op->position) {
            auto& queue = op->sending ? m_senders : m_receivers;
            queue.erase(*op->position);
            op->position.reset();
        }
        return true;
    }

    void Finish(const Op& receiver, State state)
    {
        auto sender = receiver->sender;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            receiver->state = state;
            if (sender)
                sender->state = state;
        }
        if (sender)
            Signal(sender, detail::POLL_EVENT_CUSTOM);
    }

    mutable std::mutex m_mutex;
    bool m_closed{false};
    Queue m_senders;
    Queue m_receivers;
    Buffer m_buffer;
    size_t m_head{0};
    size_t m_size{0};
    Watchers m_readWatchers;
    Watchers m_writeWatchers;
};

template <class TItem, int Max>
Chan<TItem, Max>::Chan()
    : m_core(std::make_shared<Core>())
{
}

template <class TItem, int Max> Chan<TItem, Max>::~Chan()
{
    Close();
}

template <class TItem, int Max> int Chan<TItem, Max>::Write(const ItemType& item)
{
    auto core = m_core;
    return core->Send(item, Core::Mode::Blocking);
}

template <class TItem, int Max> int Chan<TItem, Max>::TryWrite(const ItemType& item)
{
    auto core = m_core;
    return core->Send(item, Core::Mode::Immediate);
}

template <class TItem, int Max> int Chan<TItem, Max>::TryWrite(const ItemType& item, int timeout)
{
    auto core = m_core;
    return core->Send(item, Core::Mode::Timed, timeout);
}

template <class TItem, int Max> int Chan<TItem, Max>::Read(ItemType& item)
{
    auto core = m_core;
    return core->Receive([&item](const ItemType& value) { item = value; }, Core::Mode::Blocking);
}

template <class TItem, int Max> int Chan<TItem, Max>::TryRead(ItemType& item)
{
    auto core = m_core;
    return core->Receive([&item](const ItemType& value) { item = value; }, Core::Mode::Immediate);
}

template <class TItem, int Max> int Chan<TItem, Max>::TryRead(ItemType& item, int timeout)
{
    auto core = m_core;
    return core->Receive([&item](const ItemType& value) { item = value; }, Core::Mode::Timed, timeout);
}

template <class TItem, int Max> int Chan<TItem, Max>::ReadAll(std::vector<ItemType>& items)
{
    auto core = m_core;
    return core->Receive([&items](const ItemType& value) { items.push_back(value); }, Core::Mode::Blocking, 0, true);
}

template <class TItem, int Max> void Chan<TItem, Max>::Close()
{
    auto core = m_core;
    core->Close();
}

template <class TItem, int Max> bool Chan<TItem, Max>::IsClosed()
{
    return m_core->IsClosed();
}

template <class TItem, int Max> void Chan<TItem, Max>::AddReadWatcher(const CoWaiter::SPtr& waiter)
{
    m_core->AddWatcher(false, waiter);
}

template <class TItem, int Max> void Chan<TItem, Max>::RemoveReadWatcher(const CoWaiter::SPtr& waiter)
{
    m_core->RemoveWatcher(false, waiter);
}

template <class TItem, int Max> void Chan<TItem, Max>::AddWriteWatcher(const CoWaiter::SPtr& waiter)
{
    m_core->AddWatcher(true, waiter);
}

template <class TItem, int Max> void Chan<TItem, Max>::RemoveWriteWatcher(const CoWaiter::SPtr& waiter)
{
    m_core->RemoveWatcher(true, waiter);
}

template <class TItem, int Max> size_t Chan<TItem, Max>::size() const
{
    return m_core->Size();
}

template <class TItem>
Chan<TItem, 0>::Chan()
    : m_core(std::make_shared<Core>())
{
}

template <class TItem> Chan<TItem, 0>::~Chan()
{
    Close();
}

template <class TItem> int Chan<TItem, 0>::Write(const ItemType& item)
{
    auto core = m_core;
    return core->Send(item, Core::Mode::Blocking);
}

template <class TItem> int Chan<TItem, 0>::TryWrite(const ItemType& item)
{
    auto core = m_core;
    return core->Send(item, Core::Mode::Immediate);
}

template <class TItem> int Chan<TItem, 0>::TryWrite(const ItemType& item, int timeout)
{
    auto core = m_core;
    return core->Send(item, Core::Mode::Timed, timeout);
}

template <class TItem> int Chan<TItem, 0>::Read(ItemType& item)
{
    auto core = m_core;
    return core->Receive([&item](const ItemType& value) { item = value; }, Core::Mode::Blocking);
}

template <class TItem> int Chan<TItem, 0>::TryRead(ItemType& item)
{
    auto core = m_core;
    return core->Receive([&item](const ItemType& value) { item = value; }, Core::Mode::Immediate);
}

template <class TItem> int Chan<TItem, 0>::TryRead(ItemType& item, int timeout)
{
    auto core = m_core;
    return core->Receive([&item](const ItemType& value) { item = value; }, Core::Mode::Timed, timeout);
}

template <class TItem> int Chan<TItem, 0>::ReadAll(std::vector<ItemType>& items)
{
    auto core = m_core;
    /**
     * 首次配对后没有缓冲可排空。
     */
    return core->Receive([&items](const ItemType& value) { items.push_back(value); }, Core::Mode::Blocking);
}

template <class TItem> void Chan<TItem, 0>::Close()
{
    auto core = m_core;
    core->Close();
}

template <class TItem> bool Chan<TItem, 0>::IsClosed()
{
    return m_core->IsClosed();
}

} // namespace bbt::coroutine::sync
