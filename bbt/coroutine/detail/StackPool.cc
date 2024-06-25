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

StackPool::StackPool()
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
    if (m_alloc_obj_count >= m_max_size)
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
    // std::lock_guard<std::mutex> _(m_pool_mutex);
    return m_alloc_obj_count;
}


}