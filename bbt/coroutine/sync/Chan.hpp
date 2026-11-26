/**
 * @file Chan.hpp
 * @brief 协程 Channel：有缓冲与无缓冲两种，读写不满/空时挂起而不是阻塞线程。
 */
#pragma once
#include <cstddef>
#include <memory>
#include <vector>
#include <boost/noncopyable.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/sync/interface/IChan.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>

namespace bbt::coroutine::sync {

template <class TItem, int Max> class ChannelCore;
class CoSelect;

/**
 * @brief 有缓冲 Chan：满则写挂起、空则读挂起；Close 后 Read 读完残留，TryRead 直接 -1。
 *        元素按值入队（建议指针或小结构），@p Max 为容量，0 时关闭缓冲存储。
 */
template <class TItem, int Max> class Chan : public IChan<TItem>, boost::noncopyable {
public:
    typedef TItem ItemType;
    typedef std::shared_ptr<Chan> SPtr;

    /**
     * @brief 构造空缓冲 Chan，初始为打开状态。
     */
    Chan();

    /**
     * @brief 析构：未关闭时先 Close() 并唤醒等待者。
     */
    ~Chan();

    /**
     * @brief 向 Chan 写入一个元素；缓冲已满时挂起（须在协程内调用）。
     * @return 0 成功；-1 已关闭；-2 等待失败
     */
    virtual int Write(const ItemType& item) override;

    /**
     * @brief 读一个元素；缓冲空时挂起；阻塞读者按入队顺序匹配。
     * @return 0 成功；-1 已关闭且无数据；-2 等待失败
     */
    virtual int Read(ItemType& item) override;

    /**
     * @brief 读完缓冲里当前所有元素并追加到 items 末尾（不清空）；空且未关闭时挂起。
     * @return 0 成功；-1 已关闭且无数据；-2 等待失败
     */
    virtual int ReadAll(std::vector<ItemType>& items) override;

    /**
     * @brief 尝试读一个元素，不挂起；关闭时不读残留数据。
     * @return 0 成功；-1 已关闭；-2 缓冲为空
     */
    virtual int TryRead(ItemType& item) override;

    /**
     * @brief 在 timeout 毫秒内读一个元素，等待期间挂起。
     * @return 0 成功；1 超时；-1 已关闭（不读残留）；-2 等待失败
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

    /**
     * @brief 关闭 Chan：置关闭标志、唤醒所有等待者，缓冲里的数据留给读者读完。
     */
    virtual void Close() override;

    /**
     * @brief 是否已关闭；任意线程可读。
     */
    virtual bool IsClosed() override;

    /**
     * @brief 通道观察者：状态变化（可读/可写/关闭）时额外 Notify 这些 waiter。
     *
     *  与 Wait 挂起不同，watcher 不独占唤醒——Notify 时若无协程在 Wait
     *  该 waiter 则返回 -1（忽略即可）。注册后调用方须自行再 Try 一次，
     *  补上"Try 失败与注册之间"的丢唤醒窗口。CoSelect 不依赖此观察接口。
     */
    void AddReadWatcher(const CoWaiter::SPtr& waiter);

    /**
     * @brief 注销读观察者；未注册过则无操作。
     */
    void RemoveReadWatcher(const CoWaiter::SPtr& waiter);

    /**
     * @brief 注册写观察者：可写/关闭时额外 Notify 它，语义同 AddReadWatcher。
     */
    void AddWriteWatcher(const CoWaiter::SPtr& waiter);

    /**
     * @brief 注销写观察者；未注册过则无操作。
     */
    void RemoveWriteWatcher(const CoWaiter::SPtr& waiter);

    /**
     * @brief 当前缓冲队列中的元素数量
     */
    size_t size() const;

    /**
     * @brief 缓冲队列最大容量
     */
    size_t capacity() const { return Max; }

private:
    friend class CoSelect;
    using Core = ChannelCore<TItem, Max>;
    const std::shared_ptr<Core> m_core;
};

/**
 * @brief 零缓冲通道：发送与接收直接配对，等待者按各自的入队顺序匹配。
 */
template <class TItem> class Chan<TItem, 0> : private IChan<TItem>, boost::noncopyable {
public:
    using ItemType = TItem;
    using SPtr = std::shared_ptr<Chan>;

    Chan();
    ~Chan();

    int Write(const ItemType& item) override;
    int TryWrite(const ItemType& item) override;
    int TryWrite(const ItemType& item, int timeout) override;
    int Read(ItemType& item) override;
    int TryRead(ItemType& item) override;
    int TryRead(ItemType& item, int timeout) override;
    int ReadAll(std::vector<ItemType>& items) override;
    void Close() override;
    bool IsClosed() override;

private:
    friend class CoSelect;
    using Core = ChannelCore<TItem, 0>;
    const std::shared_ptr<Core> m_core;
};

/**
 * @brief Chan 写端视图：只暴露写接口，编译期拿不到 Read。
 * 与调用方共享 Chan 所有权，视图存活期间保活底层 Chan。用法：WriteChan<int, 10> w(ch); w << 42;
 */
template <class TItem, int Max> class WriteChan : boost::noncopyable {
public:
    typedef typename Chan<TItem, Max>::ItemType ItemType;

    /**
     * @brief 用已有 Chan 构造写端，与调用方共享所有权。
     */
    explicit WriteChan(std::shared_ptr<Chan<TItem, Max>> chan)
        : m_chan(chan)
    {
    }

    /**
     * @brief 转发 Chan::Write。
     */
    int Write(const ItemType& item) { return m_chan->Write(item); }

    /**
     * @brief 转发 Chan::TryWrite。
     */
    int TryWrite(const ItemType& item) { return m_chan->TryWrite(item); }

    /**
     * @brief 转发 Chan::TryWrite(ms)。
     */
    int TryWrite(const ItemType& item, int ms) { return m_chan->TryWrite(item, ms); }

    /**
     * @brief 关闭底层 Chan。
     */
    void Close() { m_chan->Close(); }

    /**
     * @brief 底层 Chan 是否已关闭。
     */
    bool IsClosed() { return m_chan->IsClosed(); }

    /**
     * @brief 写入并折算成 bool：成功为 true。
     */
    bool operator<<(const ItemType& item) { return *m_chan << item; }

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

    /**
     * @brief 用已有 Chan 构造读端，与调用方共享所有权。
     */
    explicit ReadChan(std::shared_ptr<Chan<TItem, Max>> chan)
        : m_chan(chan)
    {
    }

    /**
     * @brief 转发 Chan::Read。
     */
    int Read(ItemType& item) { return m_chan->Read(item); }

    /**
     * @brief 转发 Chan::TryRead。
     */
    int TryRead(ItemType& item) { return m_chan->TryRead(item); }

    /**
     * @brief 转发 Chan::TryRead(ms)。
     */
    int TryRead(ItemType& item, int ms) { return m_chan->TryRead(item, ms); }

    /**
     * @brief 转发 Chan::ReadAll。
     */
    int ReadAll(std::vector<ItemType>& items) { return m_chan->ReadAll(items); }

    /**
     * @brief 底层 Chan 是否已关闭。
     */
    bool IsClosed() { return m_chan->IsClosed(); }

    /**
     * @brief 读出并折算成 bool：成功为 true。
     */
    bool operator>>(ItemType& item) { return *m_chan >> item; }

private:
    std::shared_ptr<Chan<TItem, Max>> m_chan;
};

} // namespace bbt::coroutine::sync

#include <bbt/coroutine/sync/__TChan.hpp>
