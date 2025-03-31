#pragma once
#include <queue>
#include <mutex>
#include <atomic>
#include <array>
#include <boost/noncopyable.hpp>
#include <bbt/core/thread/sync/Queue.hpp>
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
     * @return 0表示成功，-1表示失败
     */
    virtual int                             Write(const ItemType& item) override;

    /**
     * @brief 从Chan（有缓冲队列）中读取一个元素
     *  此函数会导致协程挂起，当队列缓冲区为空时，读取操作会阻塞直到写端写入元素。
     *  此函数只允许一个写操作进入，当一个写操作正在进行时，其他写操作会失败。
     * @param item 读取元素
     * @return 0表示成功，-1表示失败
     */
    virtual int                             Read(ItemType& item) override;

    /**
     * @brief 尝试从Chan（有缓存）中读取一个元素，失败立即返回
     * @param item 读取元素对象
     * @return 0表示成功，-1表示失败
     */
    virtual int                             TryRead(ItemType& item) override;

    /**
     * @brief 尝试从Chan（有缓存）中读取一个元素，如果无法立即读取挂起直到超时
     * @param item 
     * @param timeout 最大挂起时间
     * @return 0表示成功，-1表示失败
     */
    virtual int                             TryRead(ItemType& item, int timeout) override;

    /**
     * @brief 尝试向Chan（有缓存）中写入一个元素，失败立即返回
     * @param item 
     * @return 0表示成功，-1表示失败 
     */
    virtual int                             TryWrite(const ItemType& item) override;

    /**
     * @brief 尝试向Chan（有缓存）中写入一个元素，如果无法立即写入挂起直到超时
     * @param item 
     * @param timeout 
     * @return 0表示成功，-1表示失败
     */
    virtual int                             TryWrite(const ItemType& item, int timeout) override;
    virtual void                            Close() override;
    virtual bool                            IsClosed() override;
protected:
    /* 可读事件 */
    int                                     _OnEnableRead();

    /**
     * @brief 
     * @return 0表示成功触发一个可写事件或没有可写事件需要触发，-1表示触发失败 
     */
    int                                     _OnEnableWrite();
protected:
    bbt::core::thread::Queue<ItemType, boost::lockfree::capacity<Max>>
                                            m_item_queue;
    volatile ChanStatus                     m_run_status{ChanStatus::CHAN_DEFAUTL};
    std::atomic_bool                        m_is_reading{false};

    /* 用来实现读写时挂起和可读写时唤醒协程 */
    CoWaiter::SPtr                          m_enable_read_cond{nullptr};
    bbt::core::thread::Queue<CoWaiter*>     m_enable_write_conds;
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
    protected Chan<TItem, 1>
{
public:
    typedef Chan<TItem, 1>                      BaseType;
    typedef typename BaseType::ItemType         ItemType;
    typedef std::shared_ptr<Chan<TItem, 0>>     SPtr;

    /**
     * @brief 向Chan中写入一个元素，阻塞直到读端读取数据
     * @param item 
     * @return 始终返回0 
     */
    virtual int                             Write(const ItemType& item) override;

    /**
     * @brief 从Chan中读取一个元素，阻塞直到写端写入数据
     * @param item 
     * @return 始终返回0
     */
    virtual int                             Read(ItemType& item) override;

    virtual void                            Close() override;
    virtual bool                            IsClosed() override;
private:
    intptr_t                                m_item_ref{nullptr};
};

} // namespace bbt::coroutine::sync

#include <bbt/coroutine/sync/__TChan.hpp>