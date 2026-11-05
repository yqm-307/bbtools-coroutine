/** @file Chan.hpp
 * @brief 协程 Channel：有缓冲与无缓冲两种，读写不满/空时挂起而不是阻塞线程。
 */
#pragma once
#include <queue>
#include <mutex>
#include <atomic>
#include <array>
#include <algorithm>
#include <boost/noncopyable.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/sync/interface/IChan.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

namespace bbt::coroutine::sync {

/**
 * @brief 有缓冲 Chan：满则写挂起、空则读挂起；Close 后 Read 读完残留，TryRead 直接 -1。
 *        元素按值入队（建议指针或小结构），@p Max 为容量，0 走下面的无缓冲特化。
 */
template <class TItem, int Max> class Chan : public IChan<TItem>, boost::noncopyable {
public:
    typedef TItem ItemType;
    typedef std::shared_ptr<Chan> SPtr;

    /// @brief 构造空缓冲 Chan，初始为打开状态。
    Chan();
    /// @brief 析构：未关闭时先 Close() 并唤醒等待者。
    ~Chan();

    /**
     * @brief 向 Chan 写入一个元素；缓冲已满时挂起（须在协程内调用）。
     * @return 0 成功；-1 已关闭；-2 等待失败
     */
    virtual int Write(const ItemType& item) override;

    /**
     * @brief 读一个元素；缓冲空时挂起。读侧单消费者，已有读者在消费则直接返回 -2。
     * @return 0 成功；-1 已关闭且无数据；-2 已有读者或等待失败
     */
    virtual int Read(ItemType& item) override;

    /**
     * @brief 读完缓冲里当前所有元素并追加到 items 末尾（不清空）；空且未关闭时挂起。
     * @return 0 成功；-1 已关闭且无数据；-2 已有读者或等待失败
     */
    virtual int ReadAll(std::vector<ItemType>& items) override;

    /**
     * @brief 尝试读一个元素，不挂起；关闭时不读残留数据。
     * @return 0 成功；-1 已关闭；-2 缓冲为空或已有读者在消费
     */
    virtual int TryRead(ItemType& item) override;

    /**
     * @brief 在 timeout 毫秒内读一个元素，等待期间挂起。
     * @return 0 成功；1 超时；-1 已关闭（不读残留）；-2 已有读者或等待失败
     */
    virtual int TryRead(ItemType& item, int timeout) override;

    /**
     * @brief 尝试写一个元素，不挂起。
     * @return 0 成功；-1 已关闭；-2 缓冲已满
     */
    virtual int TryWrite(const ItemType& item) override;

    /**
     * @brief 在 timeout 毫秒内写一个元素，等待期间挂起。
     * @return 0 成功；1 超时；-1 已关闭；-2 等待失败
     */
    virtual int TryWrite(const ItemType& item, int timeout) override;
    /// @brief 关闭 Chan：置关闭标志、唤醒所有等待者，缓冲里的数据留给读者读完。
    virtual void Close() override;
    /// @brief 是否已关闭；任意线程可读（原子标志）。
    virtual bool IsClosed() override;

    /**
     * @brief CoSelect 观察者：状态变化（可读/可写/关闭）时额外 Notify 这些 waiter。
     *
     *  与 Wait 挂起不同，watcher 不独占唤醒——Notify 时若无协程在 Wait
     *  该 waiter 则返回 -1（忽略即可）。注册后调用方须自行再 Try 一次，
     *  补上"Try 失败与注册之间"的丢唤醒窗口（CoSelect 回调里已处理）。
     */
    void AddReadWatcher(const CoWaiter::SPtr& waiter);
    /// @brief 注销读观察者；未注册过则无操作。
    void RemoveReadWatcher(const CoWaiter::SPtr& waiter);
    /// @brief 注册写观察者：可写/关闭时额外 Notify 它，语义同 AddReadWatcher。
    void AddWriteWatcher(const CoWaiter::SPtr& waiter);
    /// @brief 注销写观察者；未注册过则无操作。
    void RemoveWriteWatcher(const CoWaiter::SPtr& waiter);

    /**
     * @brief 当前缓冲队列中的元素数量
     */
    size_t size() const
    {
        std::lock_guard<std::mutex> lock(m_item_queue_mutex);
        return m_item_queue.size();
    }

    /**
     * @brief 缓冲队列最大容量
     */
    size_t capacity() const
    {
        return (size_t)m_max_size;
    }

protected:
    /**
     * @brief 挂起协程直到可读或者eof
     * @return 0表示可读，-1表示失败 
     */
    int _WaitUntilEnableRead(const detail::CoroutineOnYieldCallback& cb = nullptr);

    /**
     * @brief 挂起协程直到可读或超时
     * @param timeout_ms 最大等待的超时时间，超过此时间后即使没有可读数据，阻塞协程也会被唤醒
     * @return 0表示可读，-1表示失败，1表示超时
     */
    int _WaitUntilEnableReadOrTimeout(int timeout_ms, const detail::CoroutineOnYieldCallback& cb = nullptr);

    /**
     * @brief 挂起协程直到可写
     * @param cond 挂起事件
     * @param cb   当协程挂起成功后执行的回调事件
     * @return 0表示可写，-1表示失败
     */
    int _WaitUntilEnableWrite(CoWaiter::SPtr cond, const detail::CoroutineOnYieldCallback& cb = nullptr);

    /**
     * @brief 挂起协程直到可写或超时
     * @param cond 挂起事件
     * @param timeout_ms 最大等待的超时时间，超过此时间后即使没有可写数据，阻塞协程也会被唤醒
     * @param cb 协程挂起后回调
     * @return 0表示可写，-1表示失败，1表示超时
     */
    int _WaitUntilEnableWriteOrTimeout(CoWaiter::SPtr cond, int timeout_ms,
                                       const detail::CoroutineOnYieldCallback& cb = nullptr);

    // 唤醒读等待者并通知读观察者；返回 Notify 的结果（0 命中，-1 无等待者）。
    int _OnEnableRead();

    /**
     * @brief 唤醒一个可写等待者并通知写观察者
     * @return 0表示成功触发一个可写事件或没有可写事件需要触发，-1表示触发失败
     */
    int _OnEnableWrite();

    /* 创建一个可写等待者（未入队）；入队时机由 _RegisterEnableWriteCond 决定 */
    CoWaiter::SPtr _CreateEnableWriteCond();

    /**
     * @brief 可写等待者的「注册后入队」协议（对齐 CoCond::Wait 的 callback 入队）。
     *
     * 只允许在 CoWaiter::WaitWithCallback 的 on_registered 回调中调用——此时
     * 本 waiter 的等待事件已登记、m_co_event 可被 Notify 命中。函数持
     * m_item_queue_mutex 求值 need_wait()：仍阻塞才入队，由 _OnEnableWrite
     * 唤醒；谓词已满足（可写/已关闭）则自 Notify，走 PENDING 早到路径。
     * 这样 waiter 绝不会在「已入队但尚未可 Notify」的窗口被 pop，
     * 消除 Notify 返回 -1 造成的永久丢唤醒。
     */
    void _RegisterEnableWriteCond(const CoWaiter::SPtr& waiter, const std::function<bool()>& need_wait);

    /**
     * @brief 读侧注册后复查：ready() 为真（已可读/已关闭）则自唤醒当前读等待者。
     *
     * 只允许在 Wait 族 on_registered 回调中调用；持 m_item_queue_mutex 求值
     * ready，与写端 push+_OnEnableRead 串行化，封闭「检查→登记」窗口的数据
     * 到达丢唤醒（否则退化为 500ms 假超时）。
     */
    void _RecheckEnableRead(const std::function<bool()>& ready);

protected:
    const int m_max_size{-1};
    std::queue<ItemType> m_item_queue;
    mutable std::mutex m_item_queue_mutex; // mutable：size() const 需持锁读
    std::atomic<ChanStatus> m_run_status{ChanStatus::CHAN_DEFAUTL};
    std::atomic_bool m_is_reading{false};

    /* 用来实现读写时挂起和可读写时唤醒协程 */
    CoWaiter::SPtr m_enable_read_cond{nullptr};
    std::queue<CoWaiter::SPtr> m_enable_write_conds;

    /* CoSelect 观察者，锁语义同 m_item_queue_mutex 保护的其余成员 */
    std::vector<CoWaiter::SPtr> m_read_watchers;
    std::vector<CoWaiter::SPtr> m_write_watchers;
};

/**
 * @brief 无缓冲 Chan：Write 入队并唤醒读端；读端未就绪时挂起，关闭也会解除等待。
 * 复用 Chan<TItem, 1> 的锁与等待队列。
 */
template <class TItem>
class Chan<TItem, 0> :
    /* 复用 Chan<TItem, 1> 的锁、状态和等待队列：另写一个全新特化要复制全部同步协议，
     * 而带缓冲版本的元素队列在这里用不上。 */
    private Chan<TItem, 1> {
public:
    typedef Chan<TItem, 1> BaseType;
    typedef typename BaseType::ItemType ItemType;
    typedef std::shared_ptr<Chan<TItem, 0>> SPtr;

    // 这三个接口直接沿用带缓冲版本的实现（操作的是基类队列，不是 m_item_cache_ref）。
    using BaseType::TryRead;
    using BaseType::TryWrite;
    using BaseType::ReadAll;

    /**
     * @brief 写入一个元素；读端未就绪时挂起，0 只表示已入队或已唤醒读端，不保证已取走。
     * @return 0 已入队（读端可能尚未取走）；-1 已关闭；-2 等待失败
     */
    virtual int Write(const ItemType& item) override;

    /**
     * @brief 读一个元素并挂起，直到写端写入或 Chan 关闭。
     * @return 0 成功；-1 已关闭且无数据；-2 已有读者或等待失败
     */
    virtual int Read(ItemType& item) override;

    /// @brief 关闭 Chan：置关闭标志并唤醒读写两侧的等待者。
    virtual void Close() override;
    /// @brief 是否已关闭；任意线程可读（原子标志）。
    virtual bool IsClosed() override;

private:
    std::queue<ItemType> m_item_cache_ref;
    uint64_t m_read_idx{0};
    uint64_t m_write_idx{0};
};

/**
 * @brief Chan 写端视图：只暴露写接口，编译期拿不到 Read。
 * 与调用方共享 Chan 所有权，视图存活期间保活底层 Chan。用法：WriteChan<int, 10> w(ch); w << 42;
 */
template <class TItem, int Max> class WriteChan : boost::noncopyable {
public:
    typedef typename Chan<TItem, Max>::ItemType ItemType;

    /// @brief 用已有 Chan 构造写端，与调用方共享所有权。
    explicit WriteChan(std::shared_ptr<Chan<TItem, Max>> chan) : m_chan(chan)
    {
    }

    /// @brief 转发 Chan::Write。
    int Write(const ItemType& item)
    {
        return m_chan->Write(item);
    }
    /// @brief 转发 Chan::TryWrite。
    int TryWrite(const ItemType& item)
    {
        return m_chan->TryWrite(item);
    }
    /// @brief 转发 Chan::TryWrite(ms)。
    int TryWrite(const ItemType& item, int ms)
    {
        return m_chan->TryWrite(item, ms);
    }
    /// @brief 关闭底层 Chan。
    void Close()
    {
        m_chan->Close();
    }
    /// @brief 底层 Chan 是否已关闭。
    bool IsClosed()
    {
        return m_chan->IsClosed();
    }
    /// @brief 写入并折算成 bool：成功为 true。
    bool operator<<(const ItemType& item)
    {
        return *m_chan << item;
    }

private:
    std::shared_ptr<Chan<TItem, Max>> m_chan;
};

/**
 * @brief Chan 读端视图：只暴露读接口，编译期拿不到 Write。
 * 与调用方共享 Chan 所有权，视图存活期间保活底层 Chan。用法：ReadChan<int, 10> r(ch); r >> val;
 */
template <class TItem, int Max> class ReadChan : boost::noncopyable {
public:
    typedef typename Chan<TItem, Max>::ItemType ItemType;

    /// @brief 用已有 Chan 构造读端，与调用方共享所有权。
    explicit ReadChan(std::shared_ptr<Chan<TItem, Max>> chan) : m_chan(chan)
    {
    }

    /// @brief 转发 Chan::Read。
    int Read(ItemType& item)
    {
        return m_chan->Read(item);
    }
    /// @brief 转发 Chan::TryRead。
    int TryRead(ItemType& item)
    {
        return m_chan->TryRead(item);
    }
    /// @brief 转发 Chan::TryRead(ms)。
    int TryRead(ItemType& item, int ms)
    {
        return m_chan->TryRead(item, ms);
    }
    /// @brief 转发 Chan::ReadAll。
    int ReadAll(std::vector<ItemType>& items)
    {
        return m_chan->ReadAll(items);
    }
    /// @brief 底层 Chan 是否已关闭。
    bool IsClosed()
    {
        return m_chan->IsClosed();
    }
    /// @brief 读出并折算成 bool：成功为 true。
    bool operator>>(ItemType& item)
    {
        return *m_chan >> item;
    }

private:
    std::shared_ptr<Chan<TItem, Max>> m_chan;
};

} // namespace bbt::coroutine::sync

#include <bbt/coroutine/sync/__TChan.hpp>
