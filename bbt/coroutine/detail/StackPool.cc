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
    if (g_bbt_coroutine_config->m_cfg_static_coroutine > 0)
        _Preload(g_bbt_coroutine_config->m_cfg_static_coroutine);

}

StackPool::~StackPool()
{
    ItemType* item = nullptr; 
    /* XXX 概率返回false不为空，需要验证 */
    while (m_pool.Pop(item)) {
        delete item;
    }
}

void StackPool::Release(ItemType* item)
{
    AssertWithInfo(m_pool.Push(item), "oom!");
}

StackPool::ItemType* StackPool::Apply()
{
    if (GetCurCoNum() >= g_bbt_coroutine_config->m_cfg_stackpool_max_alloc_size)
        return nullptr;

    ItemType* item = nullptr;
    if (m_pool.Pop(item))
        return item;


    return _AllocItem();
}

int StackPool::AllocSize()
{
    return m_alloc_obj_count;
}


void StackPool::OnUpdate()
{
    if (m_co_avg == 0)
        m_co_avg = GetCurCoNum();

    if (bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(m_prev_rtts_sample_ts + bbt::core::clock::milliseconds(g_bbt_coroutine_config->m_cfg_stackpool_sample_interval))) {
        m_co_avg = ::floor(0.875 * m_co_avg + 0.125 * GetCurCoNum());
    }

    /* 通过算法计算的程序应该存在栈数量 */
    int allowable_stack_num = ::floor((m_rate + 1) * m_co_avg);

    if ( bbt::core::clock::is_expired<bbt::core::clock::milliseconds>((m_prev_adjust_pool_ts + bbt::core::clock::milliseconds(g_bbt_coroutine_config->m_cfg_stackpool_adjust_interval))))
    {
        m_prev_adjust_pool_ts = bbt::core::clock::now<>();
        /* 除了配置保证最低限度的栈数量，其余超过的栈都释放掉 */
        if (allowable_stack_num >= g_bbt_coroutine_config->m_cfg_stackpool_min_alloc_size &&
            m_alloc_obj_count > allowable_stack_num)
        {
            int delete_count = m_alloc_obj_count - allowable_stack_num;

            for (int i = 0; i < delete_count; ++i)
            {
                ItemType* item = nullptr;
                if (!m_pool.Pop(item))
                    break;

                _FreeItem(item);
            }
        }
    }
}

int StackPool::GetCurCoNum()
{
    return (m_alloc_obj_count - m_pool.Size());
}

int StackPool::GetCoAvgCount()
{
    return m_co_avg;
}

StackPool::ItemType* StackPool::_AllocItem()
{
#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_StackAlloc();
#endif
    m_alloc_obj_count++;
    return new Stack(g_bbt_coroutine_config->m_cfg_stack_size, g_bbt_coroutine_config->m_cfg_stack_protect);
}

void StackPool::_FreeItem(ItemType* item)
{
    m_alloc_obj_count--;
    delete item;
#ifdef BBT_COROUTINE_PROFILE
    g_bbt_profiler->OnEvent_StackRelease(1);
#endif
}

size_t StackPool::_Preload(size_t preload_size) noexcept
{
    size_t succ_num = 0;
    AssertWithInfo(preload_size > 0, "preload size must be greater than 0");
    for (size_t i = 0; i < preload_size; ++i) {
        auto* item = Apply();
        if (item == nullptr)
            return succ_num;;

        Release(item);
        succ_num++;
    }

    return succ_num;
}



}