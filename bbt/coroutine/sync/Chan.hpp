#pragma once
#include <queue>
#include <mutex>
#include <atomic>
#include <array>
#include <bbt/base/templateutil/Noncopyable.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/sync/interface/IChan.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>

namespace bbt::coroutine::sync
{

template<class TItem, int Max>
class Chan:
    public IChan<TItem>,
    bbt::templateutil::noncopyable
{
public:
    typedef TItem ItemType;
    typedef std::shared_ptr<Chan> SPtr;

    Chan();
    ~Chan();

    virtual int                             Write(const ItemType& item) override;
    virtual int                             Read(ItemType& item) override;
    virtual int                             ReadAll(std::vector<ItemType>& item) override;

    virtual int                             TryRead(ItemType& item) override;
    virtual int                             TryRead(ItemType& item, int timeout) override;
    virtual int                             TryWrite(const ItemType& item) override;
    virtual int                             TryWrite(const ItemType& item, int timeout) override;
    virtual void                            Close() override;
    virtual bool                            IsClosed() override;
protected:
    /**
     * @brief 挂起协程直到可读或者eof
     * @return 0表示可读，-1表示失败 
     */
    int                                     _WaitUntilEnableRead();

    /**
     * @brief 挂起协程直到可读或超时
     * @param timeout_ms 最大等待的超时时间，超过此时间后即使没有可读数据，阻塞协程也会被唤醒
     * @return 0表示可读，-1表示失败，1表示超时
     */
    int                                     _WaitUntilEnableReadOrTimeout(int timeout_ms);

    /**
     * @brief 挂起协程直到可写
     * @param cond 挂起事件
     * @return 0表示可写，-1表示失败
     */
    int                                     _WaitUntilEnableWrite(CoCond::SPtr cond);

    /**
     * @brief 挂起协程直到可写或超时
     * @param cond 挂起事件
     * @param timeout_ms 最大等待的超时时间，超过此时间后即使没有可写数据，阻塞协程也会被唤醒
     * @return 0表示可写，-1表示失败，1表示超时
     */
    int                                     _WaitUntilEnableWriteOrTimeout(CoCond::SPtr cond, int timeout_ms);

    /* 可读事件 */
    int                                     _OnEnableRead();
    /**
     * @brief 
     * @return 0表示成功触发一个可写事件或没有可写事件需要触发，-1表示触发失败 
     */
    int                                     _OnEnableWrite();
    /* 创建一个可写事件 */
    CoCond::SPtr                            _CreateAndPushEnableWriteCond();
    
private:
    const int                               m_max_size{-1};
    std::queue<ItemType>                    m_item_queue;
    std::mutex                              m_item_queue_mutex;
    volatile ChanStatus                     m_run_status{ChanStatus::CHAN_DEFAUTL};
    std::atomic_bool                        m_is_reading{false};
    /* 用来实现读写时挂起和可读写时唤醒协程 */
    CoCond::SPtr                            m_enable_read_cond{nullptr};
    std::queue<CoCond::SPtr>                m_enable_write_conds;
};

// template<class TItem, uint32_t Max = 0>
// class Chan:
//     public: Chan<class TItem, 1>
// {
// public:
//     virtual int Write(const ItemType& item) override;

// };
}

#include <bbt/coroutine/sync/__TChan.hpp>