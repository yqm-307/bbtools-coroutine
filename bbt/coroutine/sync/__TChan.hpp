#pragma once
#include <bbt/core/clock/Clock.hpp>
#include <bbt/core/util/Assert.hpp>
#include <bbt/coroutine/sync/Chan.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>


namespace bbt::coroutine::sync
{

// ============================================================
// inline helpers
// ============================================================

// RAII guard: 异常路径自动重置 m_is_reading
struct ChanReadGuard {
    std::atomic_bool& flag;
    ~ChanReadGuard() { flag.store(false, std::memory_order_release); }
};

// ============================================================
// buffered Chan (Max > 0)
// ============================================================

template<class TItem, int Max>
Chan<TItem, Max>::Chan():
    m_max_size(Max),
    m_enable_read_cond(CoWaiter::Create())
{
    Assert(m_max_size >= 0);
    m_run_status.store(ChanStatus::CHAN_OPEN, std::memory_order_release);
}

template<class TItem, int Max>
Chan<TItem, Max>::~Chan()
{
    if (!IsClosed())
        Close();
}

template<class TItem, int Max>
int Chan<TItem, Max>::Write(const ItemType& item)
{
    if (IsClosed())
        return -1;

    std::unique_lock<std::mutex> lock(m_item_queue_mutex);

    // Close() 的 CHAN_CLOSE 置位不持锁，可在首次 IsClosed() 检查与锁获取
    // 之间完成；锁内复查封闭"关闭后仍写入"窗口（Close 后 Write 必须 -1）
    if (IsClosed())
        return -1;

    while (m_item_queue.size() >= (size_t)m_max_size) {
        auto enable_write_cond = _CreateEnableWriteCond();

        lock.unlock();
        /* 注册后入队协议：waiter 只在其等待事件登记成功后的回调里才进入
         * m_enable_write_conds——否则 reader 可在 unlock→Wait 登记之间 pop
         * 它并 Notify（此时 m_co_event==nullptr，Notify 返回 -1），waiter 已
         * 离队，writer 随后永久 park。回调内持 chan 锁复查容量/关闭：条件
         * 已满足则自 Notify 走 PENDING 早到路径，不得只入队而不复查。 */
        int ret = _WaitUntilEnableWrite(enable_write_cond, [this, enable_write_cond]() {
            _RegisterEnableWriteCond(enable_write_cond, [this]() {
                return !IsClosed() && m_item_queue.size() >= (size_t)m_max_size;
            });
            return true;
        });
        if (ret != 0)
        {
            // 等待失败：若 waiter 已入队，Cancel 使其永久 Notify==-1，
            // 由 _OnEnableWrite 跳过，避免吞掉后续 writer 的唤醒
            enable_write_cond->Cancel();
            return -2;
        }

        if (IsClosed())
            return -1;

        lock.lock();
    }

    m_item_queue.push(item);
    // 读 watcher 不受 m_is_reading 门控：CoSelect 等待者不置该标志（#314）
    if (m_is_reading || !m_read_watchers.empty())
        _OnEnableRead();

    return 0;
}

template<class TItem, int Max>
int Chan<TItem, Max>::Read(ItemType& item)
{
    bool expect = false;
    if (!m_is_reading.compare_exchange_strong(expect, true))
        return -2;

    ChanReadGuard guard{m_is_reading};

    if (IsClosed()) {
        std::lock_guard<std::mutex> lock(m_item_queue_mutex);
        if (m_item_queue.empty())
            return -1;
    }

    std::unique_lock<std::mutex> lock(m_item_queue_mutex);

    while (m_item_queue.empty() && !IsClosed()) {
        lock.unlock();
        /* 注册后复查：unlock 与事件登记之间若有 push/Close 到达，其
         * _OnEnableRead 会因 waiter 尚未可 Notify 而落空；回调内持锁复查
         * 谓词，已可读/已关闭则自唤醒，避免丢唤醒退化为 500ms 假超时。 */
        int ret = _WaitUntilEnableReadOrTimeout(500, [this]() {
            _RecheckEnableRead([this]() { return !m_item_queue.empty() || IsClosed(); });
            return true;
        });
        lock.lock();
        if (ret != 0 && ret != 1)
            return -2;
    }

    if (m_item_queue.empty())
        return -1;

    item = m_item_queue.front();
    m_item_queue.pop();

    // 唤醒一个等待写入的协程（忽略 -1，可能已被 Cancel）
    _OnEnableWrite();

    return 0;
}

template<class TItem, int Max>
int Chan<TItem, Max>::ReadAll(std::vector<ItemType>& items)
{
    bool expect = false;
    if (!m_is_reading.compare_exchange_strong(expect, true))
        return -2;

    ChanReadGuard guard{m_is_reading};

    std::unique_lock<std::mutex> lock(m_item_queue_mutex);

    while (m_item_queue.empty() && !IsClosed()) {
        lock.unlock();
        /* 注册后复查：unlock 与事件登记之间若有 push/Close 到达，其
         * _OnEnableRead 会因 waiter 尚未可 Notify 而落空；回调内持锁复查
         * 谓词，已可读/已关闭则自唤醒，避免丢唤醒退化为 500ms 假超时。 */
        int ret = _WaitUntilEnableReadOrTimeout(500, [this]() {
            _RecheckEnableRead([this]() { return !m_item_queue.empty() || IsClosed(); });
            return true;
        });
        lock.lock();
        if (ret != 0 && ret != 1)
            return -2;
    }

    if (m_item_queue.empty())
        return -1;  // 关闭+空

    while (!m_item_queue.empty()) {
        items.push_back(m_item_queue.front());
        m_item_queue.pop();
        _OnEnableWrite();
    }

    return 0;
}

template<class TItem, int Max>
int Chan<TItem, Max>::TryWrite(const ItemType& item)
{
    if (IsClosed())
        return -1;

    std::lock_guard<std::mutex> lock(m_item_queue_mutex);
    if (IsClosed())
        return -1;

    if (m_item_queue.size() >= (size_t)m_max_size)
        return -2;

    m_item_queue.push(item);
    // 读 watcher 不受 m_is_reading 门控（#314）
    if (m_is_reading || !m_read_watchers.empty())
        _OnEnableRead();

    return 0;
}

template<class TItem, int Max>
int Chan<TItem, Max>::TryWrite(const ItemType& item, int timeout)
{
    if (IsClosed())
        return -1;

    auto start = bbt::core::clock::gettime_mono();
    std::unique_lock<std::mutex> lock(m_item_queue_mutex);

    // 与 Write 相同的关闭窗口：锁内复查
    if (IsClosed())
        return -1;

    while (m_item_queue.size() >= (size_t)m_max_size) {
        int elapsed = bbt::core::clock::gettime_mono() - start;
        int remaining = timeout - elapsed;
        if (remaining <= 0)
            return 1;  // 超时

        auto enable_write_cond = _CreateEnableWriteCond();
        lock.unlock();
        /* 同 Write 的注册后入队协议：waiter 只在事件登记成功后的回调里入队，
         * 并持锁复查容量/关闭，条件已满足则自唤醒（否则会假超时）。 */
        int ret = _WaitUntilEnableWriteOrTimeout(
            enable_write_cond, remaining,
            [this, enable_write_cond]() {
                _RegisterEnableWriteCond(enable_write_cond, [this]() {
                    return !IsClosed() && m_item_queue.size() >= (size_t)m_max_size;
                });
                return true;
            });

        if (ret != 0)
        {
            // 超时/失败：waiter 仍是队列里的僵尸项（COND_FREE），Cancel 后
            // _OnEnableWrite 会跳过它继续唤醒真正等待的 writer，而非停在 -1
            enable_write_cond->Cancel();
            return (ret == 1) ? 1 : -2;
        }

        if (IsClosed())
            return -1;

        lock.lock();
    }

    m_item_queue.push(item);
    // 读 watcher 不受 m_is_reading 门控（#314）
    if (m_is_reading || !m_read_watchers.empty())
        _OnEnableRead();

    return 0;
}

template<class TItem, int Max>
int Chan<TItem, Max>::TryRead(ItemType& item)
{
    if (IsClosed())
        return -1;

    bool expect = false;
    if (!m_is_reading.compare_exchange_strong(expect, true))
        return -2;

    ChanReadGuard guard{m_is_reading};

    std::lock_guard<std::mutex> lock(m_item_queue_mutex);

    if (m_item_queue.empty())
        return -2;  // 空

    item = m_item_queue.front();
    m_item_queue.pop();
    _OnEnableWrite();

    return 0;
}

template<class TItem, int Max>
int Chan<TItem, Max>::TryRead(ItemType& item, int timeout)
{
    if (IsClosed())
        return -1;

    bool expect = false;
    if (!m_is_reading.compare_exchange_strong(expect, true))
        return -2;

    ChanReadGuard guard{m_is_reading};

    auto start = bbt::core::clock::gettime_mono();
    std::unique_lock<std::mutex> lock(m_item_queue_mutex);

    while (m_item_queue.empty() && !IsClosed()) {
        int elapsed = bbt::core::clock::gettime_mono() - start;
        int remaining = timeout - elapsed;
        if (remaining <= 0)
            return 1;  // 超时

        lock.unlock();
        /* 同 Read 的注册后复查：避免 push 落在检查/登记窗口造成假超时 */
        int ret = _WaitUntilEnableReadOrTimeout(remaining,
                [this]() {
                    _RecheckEnableRead([this]() { return !m_item_queue.empty() || IsClosed(); });
                    return true;
                });
        lock.lock();
        if (ret != 0 && ret != 1)
            return -2;
    }

    if (m_item_queue.empty())
        return -1;  // 关闭+空

    item = m_item_queue.front();
    m_item_queue.pop();
    _OnEnableWrite();

    return 0;
}

template<class TItem, int Max>
void Chan<TItem, Max>::Close()
{
    if (IsClosed())
        return;

    std::lock_guard<std::mutex> lock(m_item_queue_mutex);

    // 置位在锁内：与 Write 系列"锁内复查"配对，保证关闭与入队串行化——
    // 锁内复查通过（open）后，Close 必须等该次入队完成才能置位
    m_run_status.store(ChanStatus::CHAN_CLOSE, std::memory_order_release);

    // 不再丢弃缓冲数据 — 保留给读者读完
    // 唤醒所有阻塞的协程
    if (m_is_reading)
        _OnEnableRead();

    while (!m_enable_write_conds.empty()) {
        auto enable_write_cond = m_enable_write_conds.front();
        m_enable_write_conds.pop();
        enable_write_cond->Notify();
    }

    // CoSelect watchers 必须无条件 Notify：上面的 _OnEnableRead 只在 m_is_reading
    // （有阻塞读者）时触发，select 等待者不设该标志，否则会挂在已关闭 chan 上。
    // 被唤醒方 Try 到 -1（closed）视为就绪。
    auto read_watchers = m_read_watchers;
    auto write_watchers = m_write_watchers;
    for (auto& w : read_watchers)
        w->Notify();
    for (auto& w : write_watchers)
        w->Notify();
}

template<class TItem, int Max>
bool Chan<TItem, Max>::IsClosed()
{
    return (m_run_status.load(std::memory_order_acquire) == ChanStatus::CHAN_CLOSE);
}

// ============================================================
// CoSelect watchers
// ============================================================

template<class TItem, int Max>
void Chan<TItem, Max>::AddReadWatcher(const CoWaiter::SPtr& waiter)
{
    std::lock_guard<std::mutex> lock(m_item_queue_mutex);
    m_read_watchers.push_back(waiter);
}

template<class TItem, int Max>
void Chan<TItem, Max>::RemoveReadWatcher(const CoWaiter::SPtr& waiter)
{
    std::lock_guard<std::mutex> lock(m_item_queue_mutex);
    m_read_watchers.erase(
        std::remove(m_read_watchers.begin(), m_read_watchers.end(), waiter),
        m_read_watchers.end());
}

template<class TItem, int Max>
void Chan<TItem, Max>::AddWriteWatcher(const CoWaiter::SPtr& waiter)
{
    std::lock_guard<std::mutex> lock(m_item_queue_mutex);
    m_write_watchers.push_back(waiter);
}

template<class TItem, int Max>
void Chan<TItem, Max>::RemoveWriteWatcher(const CoWaiter::SPtr& waiter)
{
    std::lock_guard<std::mutex> lock(m_item_queue_mutex);
    m_write_watchers.erase(
        std::remove(m_write_watchers.begin(), m_write_watchers.end(), waiter),
        m_write_watchers.end());
}

// ============================================================
// wait helpers (unchanged logic, adapted for unique_lock)
// ============================================================

template<class TItem, int Max>
int Chan<TItem, Max>::_WaitUntilEnableRead(const detail::CoroutineOnYieldCallback& cb)
{
    return m_enable_read_cond->WaitWithCallback(cb);
}

template<class TItem, int Max>
int Chan<TItem, Max>::_WaitUntilEnableWrite(CoWaiter::SPtr cond, const detail::CoroutineOnYieldCallback& cb)
{
    return cond->WaitWithCallback(cb);
}

template<class TItem, int Max>
int Chan<TItem, Max>::_WaitUntilEnableWriteOrTimeout(
    CoWaiter::SPtr cond, int timeout_ms, const detail::CoroutineOnYieldCallback& cb)
{
    return cond->WaitWithTimeoutAndCallback(timeout_ms, cb);
}

template<class TItem, int Max>
int Chan<TItem, Max>::_OnEnableRead()
{
    int ret = m_enable_read_cond->Notify();

    // CoSelect watchers：拷贝列表再逐个 Notify（忽略 -1：waiter 未挂起或已 Cancel）。
    // 时序约束：调用方持 m_item_queue_mutex；Notify 内部只取 waiter 自身锁并把
    // 目标协程入调度队列，不会同步 Resume、不会反向拿 chan 锁，无死锁环。
    auto watchers = m_read_watchers;
    for (auto& w : watchers)
        w->Notify();

    return ret;
}

template<class TItem, int Max>
int Chan<TItem, Max>::_OnEnableWrite()
{
    // 同 CoCond::_NotifyOne：循环 pop，跳过已失效（Notify==-1，超时/Cancel）
    // 的 waiter，直到成功唤醒一个或队列空。只 pop 一个会在僵尸 waiter 上
    // 吞掉唤醒，真正等待的 writer 永远不醒。
    int ret = 0;
    while (!m_enable_write_conds.empty())
    {
        auto enable_write_cond = m_enable_write_conds.front();
        m_enable_write_conds.pop();
        ret = enable_write_cond->Notify();
        if (ret == 0)
            break;
    }

    // CoSelect watchers：同 _OnEnableRead
    auto watchers = m_write_watchers;
    for (auto& w : watchers)
        w->Notify();

    return ret;
}

template<class TItem, int Max>
CoWaiter::SPtr Chan<TItem, Max>::_CreateEnableWriteCond()
{
    auto enable_write_cond = CoWaiter::Create();
    Assert(enable_write_cond != nullptr);
    return enable_write_cond;
}

template<class TItem, int Max>
void Chan<TItem, Max>::_RegisterEnableWriteCond(
    const CoWaiter::SPtr& waiter, const std::function<bool()>& need_wait)
{
    std::lock_guard<std::mutex> lock(m_item_queue_mutex);

    /* 谓词在锁内求值：与 reader 的 pop+_OnEnableWrite、Close 的置位+全量
     * 唤醒串行化，二者不会与本入队交错。 */
    if (need_wait())
        m_enable_write_conds.push(waiter);
    else
        /* 条件已满足（可写/已关闭）：自 Notify 走 PENDING 早到路径。
         * 此时等待事件已登记（on_registered 只在登记成功后执行），
         * Notify 必然命中，不会返回 -1。 */
        waiter->Notify();
}

template<class TItem, int Max>
void Chan<TItem, Max>::_RecheckEnableRead(const std::function<bool()>& ready)
{
    std::lock_guard<std::mutex> lock(m_item_queue_mutex);

    /* 与写端 push+_OnEnableRead 串行化：若写端先拿到锁，其 Notify 会命中
     * 已登记的本 waiter；若本复查先拿到锁，谓词为真则自唤醒。二者必居其一。 */
    if (ready())
        m_enable_read_cond->Notify();
}

template<class TItem, int Max>
int Chan<TItem, Max>::_WaitUntilEnableReadOrTimeout(
    int timeout_ms, const detail::CoroutineOnYieldCallback& cb)
{
    return m_enable_read_cond->WaitWithTimeoutAndCallback(timeout_ms, cb);
}

// ============================================================
// stream operators
// ============================================================

template<class TItem, int Max>
bool operator<<(Chan<TItem, Max>& chan, const typename Chan<TItem, Max>::ItemType& item)
{
    return (chan.Write(item) == 0);
}

template<class TItem, int Max>
bool operator>>(Chan<TItem, Max>& chan, typename Chan<TItem, Max>::ItemType& item)
{
    return (chan.Read(item) == 0);
}

template<class TItem, int Max>
bool operator<<(std::shared_ptr<Chan<TItem, Max>> chan, const TItem& item)
{
    return (chan->Write(item) == 0);
}

template<class TItem, int Max>
bool operator>>(std::shared_ptr<Chan<TItem, Max>> chan, TItem& item)
{
    return (chan->Read(item) == 0);
}

///////////////////////////////////////////////////
// Chan no cache (Chan<T, 0>)
///////////////////////////////////////////////////

template<class TItem>
int Chan<TItem, 0>::Write(const ItemType& item)
{
    if (IsClosed())
        return -1;

    std::unique_lock<std::mutex> lock(BaseType::m_item_queue_mutex);

    /* 加入引用队列，并尝试唤醒挂起的读端 */
    m_item_cache_ref.push(item);
    m_write_idx++;
    auto is_writing_idx = m_write_idx - 1;

    if (BaseType::m_is_reading && (is_writing_idx == m_read_idx)) {
        BaseType::_OnEnableRead();
        return 0;
    }

    auto enable_write_cond = BaseType::_CreateEnableWriteCond();

    lock.unlock();
    /* 同 buffered Write 的注册后入队协议；谓词为「本条数据尚未被 reader
     * 消费」（is_writing_idx 已被读取时 m_read_idx > is_writing_idx）。reader
     * 的 _OnEnableWrite 与入队持同一把锁串行化，条件已满足则自唤醒。 */
    if (BaseType::_WaitUntilEnableWrite(enable_write_cond,
            [this, is_writing_idx, enable_write_cond](){
                BaseType::_RegisterEnableWriteCond(enable_write_cond,
                    [this, is_writing_idx](){ return !IsClosed() && m_read_idx <= is_writing_idx; });
                return true;
            }) != 0)
    {
        enable_write_cond->Cancel();
        return -2;
    }

    return 0;
}

template<class TItem>
int Chan<TItem, 0>::Read(ItemType& item)
{
    bool expect = false;
    if (!BaseType::m_is_reading.compare_exchange_strong(expect, true))
        return -2;

    ChanReadGuard guard{BaseType::m_is_reading};

    if (IsClosed()) {
        std::lock_guard<std::mutex> lock(BaseType::m_item_queue_mutex);
        if (m_write_idx <= m_read_idx)
            return -1;
    }

    std::unique_lock<std::mutex> lock(BaseType::m_item_queue_mutex);

    while (m_write_idx <= m_read_idx && !IsClosed()) {
        lock.unlock();
        /* 同 buffered Read 的注册后复查：push 落在检查/登记窗口时不丢唤醒 */
        int ret = BaseType::_WaitUntilEnableReadOrTimeout(500,
                [this]() {
                    BaseType::_RecheckEnableRead(
                        [this]() { return m_write_idx > m_read_idx || IsClosed(); });
                    return true;
                });
        lock.lock();
        if (ret != 0 && ret != 1)
            return -2;
    }

    if (m_write_idx <= m_read_idx)
        return -1;  // 关闭+空

    item = m_item_cache_ref.front();
    m_item_cache_ref.pop();
    m_read_idx++;

    BaseType::_OnEnableWrite();

    return 0;
}

template<class TItem>
void Chan<TItem, 0>::Close()
{
    if (IsClosed())
        return;

    std::lock_guard<std::mutex> lock(BaseType::m_item_queue_mutex);

    // 与 Chan<TItem, Max>::Close 相同：锁内置位，与 Write 锁内复查配对
    BaseType::m_run_status.store(ChanStatus::CHAN_CLOSE, std::memory_order_release);

    if (BaseType::m_is_reading)
        BaseType::_OnEnableRead();

    while (!BaseType::m_enable_write_conds.empty()) {
        auto enable_write_cond = BaseType::m_enable_write_conds.front();
        BaseType::m_enable_write_conds.pop();
        enable_write_cond->Notify();
    }
}

template<class TItem>
bool Chan<TItem, 0>::IsClosed()
{
    return (BaseType::m_run_status.load(std::memory_order_acquire) == ChanStatus::CHAN_CLOSE);
}

} // namespace bbt::coroutine::sync
