#pragma once
#include <queue>
#include <mutex>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/base/thread/Lock.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/Stack.hpp>

namespace bbt::coroutine::detail
{

class StackPool
{
public:
    typedef std::unique_ptr<StackPool>  UPtr;
    typedef Stack                       ItemType;

    static UPtr&                        GetInstance();
    StackPool();
    ~StackPool();

    void                                Release(ItemType* item);
    ItemType*                           Apply();
    int                                 AllocSize();

    // TODO后续做定时检测并释放
    void                                OnUpdate();
    int                                 GetRtts();
protected:
    int                                 GetCurCoNum();
private:
    std::queue<ItemType*>               m_pool;                 // 空闲的stack个数  1000    ( +200 )
    bbt::thread::Spinlock               m_pool_lock;
    // std::mutex                          m_pool_mutex;
                                                                // 正在使用中       1000    (1000 - 1200) (20%)
    uint32_t                            m_alloc_obj_count{0};   // 总数             2000

    uint32_t                            m_rtts{0};              // 一段时间内，程序中平均值协程数量
    float                               m_rate{0.2};
    bbt::clock::Timestamp<>             m_prev_adjust_pool_ts;
    bbt::clock::Timestamp<>             m_prev_rtts_sample_ts;
};

}