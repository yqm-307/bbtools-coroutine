/**
 * @file __TCfsRunQueue.hpp
 * @brief CfsRunQueue 的成员实现，由 CfsRunQueue.hpp 末尾包含。
 *
 * 队列操作在每个 Processer 的调度路径上逐次调用，因此保持头内可见并显式 inline：
 * 副本随 TU 内联展开，不新增外部符号。
 */
#pragma once
#include <bbt/coroutine/detail/CfsRunQueue.hpp>

namespace bbt::coroutine::detail {

inline bool CfsRunQueue::Earlier::operator()(const CfsEntity& a, const CfsEntity& b) const noexcept
{
    return a.m_vruntime < b.m_vruntime;
}

inline bool CfsRunQueue::Enqueue(CfsEntity& entity, Coroutine* task, int priority, uint64_t weight)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (entity.is_linked() || m_running == &entity)
        return false;

    /**
     * 等待不积攒负信用；已有正债务仍保留，wake 不等于新任务。
     */
    const double debt = entity.m_waiting ? std::max(0.0, entity.m_vruntime) : entity.m_vruntime;
    entity.m_vruntime = m_floor + debt;
    entity.m_task = task;
    entity.m_priority = priority;
    entity.m_weight = std::max<uint64_t>(1, weight);
    entity.m_waiting = false;

    /**
     * upper_bound 保证相同虚拟服务量按到达顺序选择，无序号回绕。
     */
    m_queue.insert_before(m_queue.upper_bound(entity), entity);
    return true;
}

inline CfsEntity* CfsRunQueue::Pick()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running != nullptr || m_queue.empty())
        return nullptr;
    m_running = &*m_queue.begin();
    m_queue.erase(m_queue.begin());
    return m_running;
}

inline void CfsRunQueue::Account(CfsEntity& entity, uint64_t elapsed_us, bool waiting)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    /**
     * 内部唯一执行者协议损坏，不能带坏账继续发布。
     */
    if (m_running != &entity)
        std::terminate();
    const auto charge = std::max<uint64_t>(1, elapsed_us);
    entity.m_serviceUs += std::min(charge, std::numeric_limits<uint64_t>::max() - entity.m_serviceUs);
    entity.m_vruntime += static_cast<double>(charge) / entity.m_weight;
    const auto progress =
        m_queue.empty() ? entity.m_vruntime : std::min(entity.m_vruntime, m_queue.begin()->m_vruntime);
    m_floor = std::max(m_floor, progress);
    entity.m_vruntime -= m_floor;
    entity.m_waiting = waiting;

    /**
     * CommitPark 可能立即发布甚至销毁 Coroutine；不保留运行对象。
     */
    m_running = nullptr;
}

inline CfsEntity* CfsRunQueue::Take()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.empty())
        return nullptr;
    auto* entity = &*m_queue.begin();
    m_queue.erase(m_queue.begin());
    entity->m_vruntime -= m_floor;
    return entity;
}

inline size_t CfsRunQueue::Size() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

}
