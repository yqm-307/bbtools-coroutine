#pragma once
#include <queue>
#include <mutex>
#include <atomic>
#include <bbt/base/templateutil/Noncopyable.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/sync/interface/IChan.hpp>
#include <bbt/coroutine/sync/CoCond.hpp>

namespace bbt::coroutine::sync
{

template<class TItem, uint32_t Max>
class Chan:
    public IChan<TItem>,
    bbt::templateutil::noncopyable
{
public:
    typedef TItem ItemType;
    typedef std::shared_ptr<Chan> SPtr;

    Chan(int max_queue_size = 65535);
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
    /* 挂起协程直到可读或者eof */
    int                                     _WaitUntilEnableRead();
    /* 挂起协程知道可读或者超时 */
    int                                     _WaitUntilEnableReadOrTimeout(int timeout_ms);
    int                                     _WaitUntilEnableWrite();
    int                                     _WaitUntilEnableWriteOrTimeout(int timeout_ms);
    /* 可读 */
    int                                     _OnEnableRead();
    /* 可写 */
    int                                     _OnEnableWrite();
    
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

template<class TItem, uint32_t Max = 0>
class Chan:
    public: Chan<class TItem, 1>
{
public:
    virtual int Write(const ItemType& item) override;

};
}

#include <bbt/coroutine/sync/__TChan.hpp>