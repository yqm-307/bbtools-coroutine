#pragma once
#include <queue>
#include <mutex>
#include <bbt/coroutine/utils/lockfree/concurrentqueue.h>
#include <bbt/base/clock/Clock.hpp>
#include <bbt/base/thread/Lock.hpp>
#include <bbt/coroutine/detail/Define.hpp>
#include <bbt/coroutine/detail/Stack.hpp>

namespace bbt::coroutine::detail
{

/**
 * @brief 栈池
 * 
 * 使用固定栈，为了减少内存消耗，使用栈池来做栈的复用。
 * 
 * 使用了较为稳定的最大协程数算法，可以获取一个平稳值
 * 同时兼顾到内存和cpu占用。
 * 
 */
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
    moodycamel::ConcurrentQueue<ItemType*> m_pool;
    uint32_t                            m_alloc_obj_count{0};   // 总数

    uint32_t                            m_rtts{0};              // 一段时间内，程序中平均值协程数量
    float                               m_rate{0.2};
    bbt::clock::Timestamp<>             m_prev_adjust_pool_ts;
    bbt::clock::Timestamp<>             m_prev_rtts_sample_ts;
};

}