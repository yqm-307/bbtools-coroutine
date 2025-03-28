#include <cmath>
#include <bbt/core/util/Assert.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/StackPool.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>

namespace bbt::coroutine::detail
{

StackPool::UPtr& StackPool::GetInstance()
{
    static UPtr _inst = nullptr;
    if (_inst == nullptr)
        _inst = UPtr{new StackPool()};
    
    return _inst;
}

StackPool::StackPool():
    m_prev_adjust_pool_ts(bbt::core::clock::now<>())
{

}

StackPool::~StackPool()
{
    ItemType* item = nullptr; 
    /* XXX 概率返回false不为空，需要验证 */
    while (m_pool.try_dequeue(item)) {
        delete item;
    }
}

void StackPool::Release(ItemType* item)
{
    AssertWithInfo(m_pool.enqueue(item), "oom!");
}

StackPool::ItemType* StackPool::Apply()
{
    AssertWithInfo(m_alloc_obj_count < g_bbt_coroutine_config->m_cfg_stackpool_max_alloc_size,
        "coroutine stack not enough! please try adjust the globalconfig 'm_cfg_stackpool_max_alloc_size'");

    if (m_alloc_obj_count >= g_bbt_coroutine_config->m_cfg_stackpool_max_alloc_size) {
        return nullptr;
    }

    ItemType* item = nullptr;
    if (m_pool.try_dequeue(item))
        return item;

#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_StackAlloc();
#endif
    m_alloc_obj_count++;
    return new Stack(g_bbt_coroutine_config->m_cfg_stack_size, g_bbt_coroutine_config->m_cfg_stack_protect);
}

int StackPool::AllocSize()
{
    return m_alloc_obj_count;
}


void StackPool::OnUpdate()
{
    if (m_rtts == 0)
        m_rtts = GetCurCoNum();

    if (bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(m_prev_rtts_sample_ts + bbt::core::clock::milliseconds(g_bbt_coroutine_config->m_cfg_stackpool_sample_interval))) {
        m_rtts = ::floor(0.875 * m_rtts + 0.125 * GetCurCoNum());
    }

    /* 通过算法计算的程序应该存在栈数量 */
    int allowable_stack_num = ::floor((m_rate + 1) * m_rtts);

    if ( bbt::core::clock::is_expired<bbt::core::clock::milliseconds>((m_prev_adjust_pool_ts + bbt::core::clock::milliseconds(g_bbt_coroutine_config->m_cfg_stackpool_adjust_interval))))
    {
        m_prev_adjust_pool_ts = bbt::core::clock::now<>();
        /* 除了配置保证最低限度的栈数量，其余超过的栈都释放掉 */
        if (allowable_stack_num >= g_bbt_coroutine_config->m_cfg_stackpool_min_alloc_size &&
            m_alloc_obj_count > allowable_stack_num)
        {
            decltype(m_pool) m_swap_queue;
            
            m_swap_queue.swap(m_pool);
            int count = m_swap_queue.size_approx();

#ifdef BBT_COROUTINE_PROFILE
            g_bbt_profiler->OnEvent_StackRelease(count);
#endif
            m_alloc_obj_count -= count;

            ItemType* item = nullptr;
            while (m_swap_queue.try_dequeue(item))
                delete item;

        }
    }
}

int StackPool::GetCurCoNum()
{
    return (m_alloc_obj_count - m_pool.size_approx());
}

int StackPool::GetRtts()
{
    return m_rtts;
}


}