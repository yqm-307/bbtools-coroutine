#pragma once
#include <queue>
#include <mutex>
#include <bbt/core/thread/sync/Queue.hpp>
#include <bbt/core/clock/Clock.hpp>
#include <bbt/core/thread/Lock.hpp>
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
    /* 定时更新，采样、动态调整池大小 */
    void                                OnUpdate();

    /* 归还Stack */
    void                                Release(ItemType* item);
    /* 申请Stack */
    ItemType*                           Apply();
    /* 已经创建的Stack总数，非线程安全的参考值 */
    int                                 AllocSize();
    /* 最近使用Co的平均值，非线程安全的参考值 */
    int                                 GetCoAvgCount();
    /* 当前内存中Co数量，非线程安全的参考值 */
    int                                 GetCurCoNum();
protected:
    ItemType*                           _AllocItem();
    void                                _FreeItem(ItemType* item);
private:
    bbt::core::thread::Queue<ItemType*> m_pool{1024};
    std::atomic_uint32_t                m_alloc_obj_count{0};   // 总数

    std::atomic_uint32_t                m_co_avg{0};              // 一段时间内，程序中平均值协程数量
    const float                         m_rate{0};
    bbt::core::clock::Timestamp<>       m_prev_adjust_pool_ts;
    bbt::core::clock::Timestamp<>       m_prev_rtts_sample_ts;
};

}