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

template<class TItem>
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
    virtual void                            Close() override;
    virtual bool                            IsClosed() override;
protected:
    int                                     _Wait();
    int                                     _WaitWithTimeout(int timeout_ms);
    int                                     _Notify();
private:
    const int                               m_max_size{-1};
    std::queue<ItemType>                    m_item_queue;
    std::mutex                              m_item_queue_mutex;
    volatile ChanStatus                     m_run_status{ChanStatus::CHAN_DEFAUTL};
    std::atomic_bool                        m_is_reading{false};
    CoCond::SPtr                            m_cond{nullptr};
};

}

#include <bbt/coroutine/sync/__TChan.hpp>