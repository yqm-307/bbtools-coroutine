#pragma once
#include <queue>
#include <mutex>
#include <atomic>
#include <array>
#include <boost/noncopyable.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/sync/interface/IChan.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

namespace bbt::coroutine::sync
{


/**
 * @brief 有缓冲队列的Chan
 * 
 *  Chan提供了多协程之间的交互功能。提供了Read、Write接口
 *  提供了TryWrite、TryRead接口。可以不挂起当前协程的进行
 *  读写。
 * 
 *  Write族函数和Read族函数有特性：
 *      （1）Write族函数：无法写入会挂起直到可写。
 *      （2）Read族函数：无法读取会挂起直到可读
 *  ps：无法读取的原因：缓冲区满（空）、Chan关闭
 * 
 * @tparam TItem 元素类型，建议不要使用平凡类类型，推荐使用指针类型 
 * @tparam Max 缓冲队列最大长度，0 < Max < (1 << 32 - 1)
 */
template<class TItem, int Max>
class Chan:
    public IChan<TItem>,
    boost::noncopyable
{
public:
    typedef TItem ItemType;
    typedef std::shared_ptr<Chan> SPtr;

    Chan();
    ~Chan();

    /**
     * @brief 向Chan（有缓冲队列）中写入一个元素
     *  此函数会导致协程挂起，当队列缓冲区已满时，写入操作会导致协程挂起，直到读端读取元素
     *  导致可写。
     * @param item 要写入Chan的元素
     * @return 0表示成功，-1表示Chan已关闭且无数据，-2表示错误
     */
    virtual int                             Write(const ItemType& item) override;

    /**
     * @brief 从Chan（有缓冲队列）中读取一个元素
     *  此函数会导致协程挂起，当队列缓冲区为空时，读取操作会阻塞直到写端写入元素。
     *  此函数只允许一个写操作进入，当一个写操作正在进行时，其他写操作会失败。
     * @param item 读取元素
     * @return 0表示成功，-1表示Chan已关闭且无数据，-2表示错误
     */
    virtual int                             Read(ItemType& item) override;

    /**
     * @brief 从Chan（有缓冲队列）中读取所有元素
     *  此函数会导致协程挂起
     * @param items 读取元素数组
     * @return 0表示成功，-1表示Chan已关闭且无数据，-2表示错误
     */
    virtual int                             ReadAll(std::vector<ItemType>& items) override;

    /**
     * @brief 尝试从Chan（有缓存）中读取一个元素，失败立即返回
     * @param item 读取元素对象
     * @return 0表示成功，-1表示Chan已关闭且无数据，-2表示错误
     */
    virtual int                             TryRead(ItemType& item) override;

    /**
     * @brief 尝试从Chan（有缓存）中读取一个元素，如果无法立即读取挂起直到超时
     * @param item 
     * @param timeout 最大挂起时间
     * @return 0表示成功，-1表示Chan已关闭且无数据，-2表示错误
     */
    virtual int                             TryRead(ItemType& item, int timeout) override;

    /**
     * @brief 尝试向Chan（有缓存）中写入一个元素，失败立即返回
     * @param item 
     * @return 0表示成功，-1表示Chan已关闭且无数据，-2表示错误 
     */
    virtual int                             TryWrite(const ItemType& item) override;

    /**
     * @brief 尝试向Chan（有缓存）中写入一个元素，如果无法立即写入挂起直到超时
     * @param item 
     * @param timeout 
     * @return 0表示成功，-1表示Chan已关闭且无数据，-2表示错误
     */
    virtual int                             TryWrite(const ItemType& item, int timeout) override;
    virtual void                            Close() override;
    virtual bool                            IsClosed() override;

    /**
     * @brief 当前缓冲队列中的元素数量
     */
    size_t                                  size() const { return m_item_queue.size(); }

    /**
     * @brief 缓冲队列最大容量
     */
    size_t                                  capacity() const { return (size_t)m_max_size; }
protected:
    /**
     * @brief 挂起协程直到可读或者eof
     * @return 0表示可读，-1表示失败 
     */
    int                                     _WaitUntilEnableRead(const detail::CoroutineOnYieldCallback& cb = nullptr);

    /**
     * @brief 挂起协程直到可读或超时
     * @param timeout_ms 最大等待的超时时间，超过此时间后即使没有可读数据，阻塞协程也会被唤醒
     * @return 0表示可读，-1表示失败，1表示超时
     */
    int                                     _WaitUntilEnableReadOrTimeout(int timeout_ms, const detail::CoroutineOnYieldCallback& cb = nullptr);

    /**
     * @brief 挂起协程直到可写
     * @param cond 挂起事件
     * @param cb   当协程挂起成功后执行的回调事件
     * @return 0表示可写，-1表示失败
     */
    int                                     _WaitUntilEnableWrite(CoWaiter::SPtr cond, const detail::CoroutineOnYieldCallback& cb = nullptr);

    /**
     * @brief 挂起协程直到可写或超时
     * @param cond 挂起事件
     * @param timeout_ms 最大等待的超时时间，超过此时间后即使没有可写数据，阻塞协程也会被唤醒
     * @param cb 协程挂起后回调
     * @return 0表示可写，-1表示失败，1表示超时
     */
    int                                     _WaitUntilEnableWriteOrTimeout(CoWaiter::SPtr cond, int timeout_ms, const detail::CoroutineOnYieldCallback& cb = nullptr);

    /* 可读事件 */
    int                                     _OnEnableRead();

    /**
     * @brief 
     * @return 0表示成功触发一个可写事件或没有可写事件需要触发，-1表示触发失败 
     */
    int                                     _OnEnableWrite();

    /* 创建一个可写事件 */
    CoWaiter::SPtr                            _CreateAndPushEnableWriteCond();

    void                                    _Lock();
    void                                    _UnLock();
protected:
    const int                               m_max_size{-1};
    std::queue<ItemType>                    m_item_queue;
    std::mutex                              m_item_queue_mutex;
    volatile ChanStatus                     m_run_status{ChanStatus::CHAN_DEFAUTL};
    std::atomic_bool                        m_is_reading{false};

    /* 用来实现读写时挂起和可读写时唤醒协程 */
    CoWaiter::SPtr                            m_enable_read_cond{nullptr};
    std::queue<CoWaiter::SPtr>                m_enable_write_conds;
};


/**
 * @brief 无缓冲队列的Chan
 *  
 * @tparam TItem 
 */
template<class TItem>
class Chan<TItem, 0>:
    /** 
     * XXX 这里使用了取巧的方式。
     * 
     * 最正派的方式还是特例化一个全新的Chan，这样
     * 运行时效率最高，没有继承带来的开销了
     * 
     * 为什么不在带缓冲队列的Chan适配？
     * 感觉代码会不好看，而且如何暴露接口也是个问题。
     */
    private Chan<TItem, 1>
{
public:
    typedef Chan<TItem, 1>                      BaseType;
    typedef typename BaseType::ItemType         ItemType;
    typedef std::shared_ptr<Chan<TItem, 0>>     SPtr;

    using BaseType::TryRead;
    using BaseType::TryWrite;
    using BaseType::ReadAll;

    /**
     * @brief 向Chan中写入一个元素，阻塞直到读端读取数据
     * @param item 
     * @return 0表示成功，-1表示Chan已关闭且无数据，-2表示错误
     */
    virtual int                             Write(const ItemType& item) override;

    /**
     * @brief 从Chan中读取一个元素，阻塞直到写端写入数据
     * @param item 
     * @return 0表示成功，-1表示Chan已关闭且无数据，-2表示错误
     */
    virtual int                             Read(ItemType& item) override;

    virtual void                            Close() override;
    virtual bool                            IsClosed() override;
private:
    std::queue<ItemType>                    m_item_cache_ref;
    uint64_t                                m_read_idx{0};
    uint64_t                                m_write_idx{0};
};

/**
 * @brief Chan 写端 — 编译期禁止 Read
 * 
 * 用法：WriteChan<int, 10> w(ch);  w << 42;
 */
template<class TItem, int Max>
class WriteChan : boost::noncopyable {
public:
    typedef typename Chan<TItem, Max>::ItemType ItemType;

    explicit WriteChan(std::shared_ptr<Chan<TItem, Max>> chan) : m_chan(chan) {}

    int Write(const ItemType& item)          { return m_chan->Write(item); }
    int TryWrite(const ItemType& item)       { return m_chan->TryWrite(item); }
    int TryWrite(const ItemType& item, int ms) { return m_chan->TryWrite(item, ms); }
    void Close()                             { m_chan->Close(); }
    bool IsClosed()                          { return m_chan->IsClosed(); }
    bool operator<<(const ItemType& item)    { return *m_chan << item; }

private:
    std::shared_ptr<Chan<TItem, Max>> m_chan;
};

/**
 * @brief Chan 读端 — 编译期禁止 Write
 * 
 * 用法：ReadChan<int, 10> r(ch);  r >> val;  if (r.Read(val) == -1) // closed
 */
template<class TItem, int Max>
class ReadChan : boost::noncopyable {
public:
    typedef typename Chan<TItem, Max>::ItemType ItemType;

    explicit ReadChan(std::shared_ptr<Chan<TItem, Max>> chan) : m_chan(chan) {}

    int Read(ItemType& item)                 { return m_chan->Read(item); }
    int TryRead(ItemType& item)              { return m_chan->TryRead(item); }
    int TryRead(ItemType& item, int ms)      { return m_chan->TryRead(item, ms); }
    int ReadAll(std::vector<ItemType>& items){ return m_chan->ReadAll(items); }
    bool IsClosed()                          { return m_chan->IsClosed(); }
    bool operator>>(ItemType& item)          { return *m_chan >> item; }

private:
    std::shared_ptr<Chan<TItem, Max>> m_chan;
};

} // namespace bbt::coroutine::sync

#include <bbt/coroutine/sync/__TChan.hpp>
