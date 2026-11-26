/**
 * @file __TChan.hpp
 * @brief Chan 的模板实现，由 Chan.hpp 末尾包含。
 */
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
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

/**
 * 一个 Run 的首胜记录。ChannelCore 先持通道锁，再按地址序取至多两个 Run 锁；
 * 中断只取 Run 锁，不回调通道。普通操作传空记录，不取 Run 锁。
 */
class SelectChoice {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr int Pending = -2;

    void SetDeadline(Clock::time_point deadline)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_deadline = deadline;
    }

    int Winner()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_winner;
    }

    bool Interrupt()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_winner != Pending)
            return false;
        m_winner = -1;
        return true;
    }

    static bool Claim(SelectChoice* first, int index, SelectChoice* second, int peer_index, bool commit)
    {
        if (!first && !second)
            return true;
        if (first == second)
            return false;
        if (!first || (second && std::less<SelectChoice*>{}(second, first))) {
            std::swap(first, second);
            std::swap(index, peer_index);
        }
        std::unique_lock<std::mutex> lock(first->m_mutex);
        std::unique_lock<std::mutex> peer_lock;
        if (second)
            peer_lock = std::unique_lock<std::mutex>(second->m_mutex);
        const auto now = Clock::now();
        if (!first->CanClaim(now) || (second && !second->CanClaim(now)))
            return false;
        if (commit) {
            first->m_winner = index;
            if (second)
                second->m_winner = peer_index;
        }
        return true;
    }

private:
    bool CanClaim(Clock::time_point now) const { return m_winner == Pending && now < m_deadline; }

    std::mutex m_mutex;
    Clock::time_point m_deadline{Clock::time_point::max()};
    int m_winner{Pending};
};

template <class TItem, int Max> class ChannelCore : public std::enable_shared_from_this<ChannelCore<TItem, Max>> {
    friend class CoSelect;
    static_assert(Max >= 0, "Channel capacity must be non-negative");
    enum class State { Pending, Matched, Completed, Failed, Closed, Cancelled, TimedOut, Unavailable };
    enum class Attempt { Done, Prepare, Arm, Register, Park };
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
         * 只在通道锁内预留或转移值槽。晚到的唤醒和写回执不再访问该槽。
         */
        Value& value;
        const Clock::time_point deadline;
        Batch* batch;
        const bool drain_on_close;
        State state{State::Pending};
        std::optional<typename Queue::iterator> position;
        Wait wait;
        Op sender;
        std::shared_ptr<SelectChoice> choice;
        int index{-1};
        uint64_t enqueue_order{0};
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

        /**
         * 缓冲即时写必然在 Admit 内完成（不挂起、不入队），用栈 Operation 即可，
         * 省去共享等待记录与读取结果的再次加锁；元素复制仍在锁外。
         */
        if constexpr (Max > 0) {
            if (mode == Mode::Immediate) {
                auto value = std::make_unique<TItem>(item);
                Operation op(true, value, Clock::time_point::max());
                Admit(op, Op{}, co, mode, Wait{});
                return Result(op.state);
            }
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
        /**
         * 缓冲即时读（含空读探测）必然在 Admit 内完成，用栈 Operation 单锁即可。
         * 命中后锁外 deliver；缓冲读无写者回执，赋值抛异常时值已出队不回队，直接外抛。
         */
        if constexpr (Max > 0) {
            if (mode == Mode::Immediate && !all) {
                Value value;
                Operation op(false, value, Clock::time_point::max());
                Admit(op, Op{}, CurrentCoroutine(), mode, Wait{});
                if (op.state != State::Matched)
                    return Result(op.state);
                deliver(std::as_const(*value));
                return Result(State::Completed);
            }
        }
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
        DeliverMatched(op, [&]() {
            deliver(std::as_const(*value));
            for (const auto& entry : batch)
                deliver(std::as_const(*entry));
        });
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
            if constexpr (Max > 0) {
                /**
                 * 正常配对时等待接收者与缓冲不会并存；关闭仍按同一排空规则处理，
                 * 且已拒绝的写者不能在排空过程中补位。
                 */
                for (auto it = m_receivers.begin(); m_size != 0 && it != m_receivers.end();) {
                    auto current = it++;
                    if (!(*current)->drain_on_close)
                        continue;
                    ReserveBuffered(**current);
                    (*current)->position.reset();
                    matched.splice(matched.end(), m_receivers, current);
                }
            }
            /**
             * Close 同时使读写就绪；同通道的分支按发布顺序竞争，而不是固定由写分支抢先。
             */
            m_senders.merge(m_receivers, [](const Op& a, const Op& b) { return a->enqueue_order < b->enqueue_order; });
            ClosePending(m_senders, wake);
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

    static bool Claim(Operation& op, Operation* peer = nullptr, bool commit = true)
    {
        return SelectChoice::Claim(
            op.choice.get(), op.index, peer ? peer->choice.get() : nullptr, peer ? peer->index : -1, commit);
    }

    static void ClosePending(Queue& queue, Queue& wake)
    {
        while (!queue.empty()) {
            const auto& op = queue.front();
            op->position.reset();
            if (Claim(*op)) {
                op->state = State::Closed;
                wake.splice(wake.end(), queue, queue.begin());
            } else {
                queue.pop_front();
            }
        }
    }

    void Start(const Op& op, detail::Coroutine* co, Mode mode)
    {
        const auto attempt = Admit(*op, op, co, mode, {});
        if (attempt != Attempt::Register && attempt != Attempt::Arm)
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
                [&](const Wait& wait) { return Admit(*op, op, co, mode, wait) == Attempt::Park; })) {
            Resolve(op, State::Unavailable);
        }
    }

    Attempt
    Admit(Operation& op, const Op& owned, detail::Coroutine* co, Mode mode, const Wait& wait, Queue* prepared = nullptr)
    {
        Queue expired;
        Queue pending;
        Queue admitted;
        if (wait && !prepared)
            pending.push_back(owned);
        auto& publication = prepared ? *prepared : pending;
        Op peer_to_wake;
        Attempt next;
        bool wake_self = false;
        {
            detail::DriverWakeDeferral defer;
            std::lock_guard<std::mutex> lock(m_mutex);
            const bool publishing = op.choice && !publication.empty();
            next = AdmitLocked(op, owned, co, mode, wait, publication, expired, admitted, peer_to_wake);
            wake_self = publishing && next == Attempt::Done
                        && (op.state == State::Closed || op.state == State::Matched || op.state == State::Completed);
        }
        SignalSends(expired, admitted);
        if (peer_to_wake)
            Signal(peer_to_wake, detail::POLL_EVENT_CUSTOM);
        if (wake_self)
            wait->Trigger(detail::POLL_EVENT_CUSTOM);
        return next;
    }

    Attempt AdmitLocked(
        Operation& op,
        [[maybe_unused]] const Op& owned,
        detail::Coroutine* co,
        Mode mode,
        const Wait& wait,
        Queue& pending,
        Queue& expired,
        Queue& admitted,
        Op& peer_to_wake)
    {
        if (op.state != State::Pending)
            return Attempt::Done;
        /**
         * 缓冲残留只允许 Read / ReadAll 排空；两种 TryRead 都将关闭视为终态。
         */
        if (m_closed && (!op.drain_on_close || m_size == 0)) {
            if (Claim(op))
                op.state = State::Closed;
            return Attempt::Done;
        }
        if (co && co->IsCancelRequested()) {
            op.state = State::Cancelled;
            return Attempt::Done;
        }
        const auto now = Clock::now();
        if (op.deadline <= now) {
            op.state = State::TimedOut;
            return Attempt::Done;
        }
        if (op.choice && !Claim(op, nullptr, false))
            return Attempt::Done;

        if constexpr (Max > 0) {
            if (!op.sending && m_size != 0) {
                if (!Claim(op))
                    return Attempt::Done;
                ReserveBuffered(op);

                /**
                 * 先为本次读（含 ReadAll 快照）保留旧值，再按 FIFO 给等待写者补位。
                 * 补位仍在同一锁内，避免后来的 TryWrite 抢走刚释放的容量。
                 */
                OnBufferSpace(now, expired, admitted);
                return Attempt::Done;
            }
        }

        auto& peers = op.sending ? m_receivers : m_senders;
        const bool await_delivery = Max == 0 && op.sending && mode != Mode::Immediate;
        for (auto it = peers.begin(); it != peers.end();) {
            auto current = it++;
            if (Expire(peers, current, now, expired))
                continue;
            const bool prepare = op.sending && !op.value;
            if (!Claim(op, current->get(), !prepare && (!await_delivery || wait)))
                continue;
            if (prepare)
                return Attempt::Prepare;
            if (await_delivery && !wait)
                return Attempt::Arm;
            auto peer = std::move(*current);
            peers.erase(current);
            peer->position.reset();
            /**
             * 有缓冲写入在接纳值时完成，即使等待读者直接取得该值。
             * 只有零缓冲交接的阻塞写者等待接收端赋值结果。
             */
            if (op.sending) {
                peer->value = std::move(op.value);
                peer->state = State::Matched;
                if constexpr (Max == 0) {
                    op.wait = wait;
                    op.state = State::Matched;
                    peer->sender = owned;
                } else {
                    op.state = State::Completed;
                }
                peer_to_wake = std::move(peer);
            } else {
                op.value = std::move(peer->value);
                op.state = State::Matched;
                if constexpr (Max == 0) {
                    op.wait = wait;
                    peer->state = State::Matched;
                    op.sender = peer;
                } else {
                    peer->state = State::Completed;
                    peer_to_wake = std::move(peer);
                }
            }
            return await_delivery ? Attempt::Park : Attempt::Done;
        }

        if constexpr (Max > 0) {
            if (op.sending && m_size < Max) {
                if (!Claim(op, nullptr, static_cast<bool>(op.value)))
                    return Attempt::Done;
                if (!op.value)
                    return Attempt::Prepare;
                PushBuffered(std::move(op.value));
                op.state = State::Completed;
                NotifyWatchers(m_readWatchers);
                return Attempt::Done;
            }
        }

        if (op.choice && pending.empty())
            return Attempt::Register;
        if (mode == Mode::Immediate || (mode == Mode::Timed && op.deadline == Clock::time_point::max())) {
            op.state = mode == Mode::Immediate ? State::Unavailable : State::TimedOut;
            return Attempt::Done;
        }
        if (!wait)
            return Attempt::Register;
        auto& queue = op.sending ? m_senders : m_receivers;
        op.position = pending.begin();
        op.enqueue_order = m_next_enqueue++;
        queue.splice(queue.end(), pending);
        op.wait = wait;
        return Attempt::Park;
    }

    static bool Expire(Queue& queue, typename Queue::iterator position, Clock::time_point now, Queue& expired)
    {
        /**
         * 定时器投递延迟不延长已入队操作的匹配期限。
         */
        const auto& op = *position;
        if (op->deadline > now)
            return false;
        op->state = State::TimedOut;
        op->position.reset();
        expired.splice(expired.end(), queue, position);
        return true;
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

    void ReserveBuffered(Operation& receiver)
    {
        receiver.value = PopBuffered();
        ReserveBatchTail(receiver, Max - 1);
        receiver.state = State::Matched;
    }

    void ReserveBatchTail(Operation& receiver, size_t limit)
    {
        if (auto* batch = std::exchange(receiver.batch, nullptr)) {
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
            ReserveBatchTail(*receiver, Max - 1);
            OnBufferSpace(Clock::now(), expired, admitted);
        }
        SignalSends(expired, admitted);
    }

    void OnBufferSpace(Clock::time_point now, Queue& expired, Queue& admitted)
    {
        if (m_closed)
            return;
        for (auto it = m_senders.begin(); m_size < Max && it != m_senders.end();) {
            auto current = it++;
            if (Expire(m_senders, current, now, expired))
                continue;
            const auto& sender = *current;
            if (!Claim(*sender))
                continue;
            PushBuffered(std::move(sender->value));
            sender->state = State::Completed;
            sender->position.reset();
            admitted.splice(admitted.end(), m_senders, current);
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

    State ReadState(const Op& op)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return op->state;
    }

    template <class Delivery> void DeliverMatched(const Op& receiver, Delivery deliver)
    {
        try {
            deliver();
        } catch (...) {
            /**
             * 配对后容量可能已被下一位写者占用；已保留的值不能回队。
             */
            Finish(receiver, State::Failed);
            throw;
        }
        Finish(receiver, State::Completed);
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
    uint64_t m_next_enqueue{0};
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
