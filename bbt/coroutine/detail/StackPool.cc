#include <cmath>
#include <bbt/base/assert/Assert.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <bbt/coroutine/detail/StackPool.hpp>

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
    m_prev_adjust_pool_ts(bbt::clock::now<>())
{

}

StackPool::~StackPool()
{

}

void StackPool::Release(ItemType* item)
{
    std::lock_guard<std::mutex> _(m_pool_mutex);
    m_pool.push(item);
}

StackPool::ItemType* StackPool::Apply()
{
    std::lock_guard<std::mutex> _(m_pool_mutex);
    if (m_alloc_obj_count >= g_bbt_coroutine_config->m_cfg_stackpool_max_alloc_size)
        return nullptr;

    if (m_pool.empty()) {
        m_alloc_obj_count++;
        return new Stack(g_bbt_coroutine_config->m_cfg_stack_size, g_bbt_coroutine_config->m_cfg_stack_protect);
    }

    auto ret = m_pool.front();
    m_pool.pop();
    return ret;
}

int StackPool::AllocSize()
{
    std::lock_guard<std::mutex> _(m_pool_mutex);
    return m_alloc_obj_count;
}


void StackPool::OnUpdate()
{
    if (m_rtts == 0)
        m_rtts = GetCurCoNum();
    
    m_rtts = ::floor(0.875 * m_rtts + 0.125 * GetCurCoNum());
    int allowable_stack_num = ::floor((m_rate + 1) * m_rtts);

    if ( bbt::clock::is_expired<bbt::clock::milliseconds>((m_prev_adjust_pool_ts + bbt::clock::milliseconds(500))))
    {
        m_prev_adjust_pool_ts = bbt::clock::now<>();
        if (m_alloc_obj_count > allowable_stack_num)
        {
            std::queue<ItemType*> m_swap_queue;
            {
                std::lock_guard<std::mutex> _(m_pool_mutex);
                m_swap_queue.swap(m_pool);
                Assert(m_pool.size() == 0);
                m_alloc_obj_count -= m_swap_queue.size();
            }

            while (!m_swap_queue.empty())
            {
                delete m_swap_queue.front();
                m_swap_queue.pop();
            }
        }
    }
}

int StackPool::GetCurCoNum()
{
    std::lock_guard<std::mutex> _(m_pool_mutex);
    return (m_alloc_obj_count - m_pool.size());
}

int StackPool::GetRtts()
{
    return m_rtts;
}


}