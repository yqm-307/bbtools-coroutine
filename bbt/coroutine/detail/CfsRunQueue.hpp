#pragma once

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <boost/intrusive/set.hpp>

namespace bbt::coroutine::detail
{
class Coroutine;
class CfsRunQueue;

// 节点随 Coroutine 生灭；不保有 P。脱离队列时 vruntime 是相对源 floor 的债务。
class CfsEntity : public boost::intrusive::set_base_hook<>
{
public:
    CfsEntity() = default;
    CfsEntity(const CfsEntity&) = delete;
    CfsEntity& operator=(const CfsEntity&) = delete;
    uint64_t ServiceUs() const noexcept { return m_serviceUs; }
    Coroutine* Task() const noexcept { return m_task; }
    int Priority() const noexcept { return m_priority; }
private:
    friend class CfsRunQueue;
    Coroutine* m_task{nullptr};
    uint64_t m_serviceUs{0};
    double m_vruntime{0};
    uint64_t m_weight{1};
    int m_priority{0};
    bool m_waiting{false};
};

// 纯调度边界：不执行 Resume/Wake，不分配节点。所有集合、floor 和统计受同一锁保护。
class CfsRunQueue
{
    struct Earlier
    {
        bool operator()(const CfsEntity& a, const CfsEntity& b) const noexcept
        { return a.m_vruntime < b.m_vruntime; }
    };
    using Queue = boost::intrusive::multiset<CfsEntity,
        boost::intrusive::compare<Earlier>>;
public:
    bool Enqueue(CfsEntity& entity, Coroutine* task, int priority, uint64_t weight)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (entity.is_linked() || m_running == &entity)
            return false;
        // 等待不积攒负信用；已有正债务仍保留，wake 不等于新任务。
        const double debt = entity.m_waiting ? std::max(0.0, entity.m_vruntime) : entity.m_vruntime;
        entity.m_vruntime = m_floor + debt;
        entity.m_task = task;
        entity.m_priority = priority;
        entity.m_weight = std::max<uint64_t>(1, weight);
        entity.m_waiting = false;
        // upper_bound 保证相同虚拟服务量按到达顺序选择，无序号回绕。
        m_queue.insert_before(m_queue.upper_bound(entity), entity);
        return true;
    }

    CfsEntity* Pick()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running != nullptr || m_queue.empty())
            return nullptr;
        m_running = &*m_queue.begin();
        m_queue.erase(m_queue.begin());
        return m_running;
    }

    void Account(CfsEntity& entity, uint64_t elapsed_us, bool waiting)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running != &entity)
            std::terminate(); // 内部唯一执行者协议损坏，不能带坏账继续发布。
        const auto charge = std::max<uint64_t>(1, elapsed_us);
        entity.m_serviceUs += std::min(charge,
            std::numeric_limits<uint64_t>::max() - entity.m_serviceUs);
        entity.m_vruntime += static_cast<double>(charge) / entity.m_weight;
        const auto progress = m_queue.empty() ? entity.m_vruntime
            : std::min(entity.m_vruntime, m_queue.begin()->m_vruntime);
        m_floor = std::max(m_floor, progress);
        entity.m_vruntime -= m_floor;
        entity.m_waiting = waiting;
        // CommitPark 可能立即发布甚至销毁 Coroutine；不保留运行对象。
        m_running = nullptr;
    }

    // steal/retire 只导出 runnable；与 Pick/Account 共用源锁，目标锁由调用方稍后取得。
    CfsEntity* Take()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty())
            return nullptr;
        auto* entity = &*m_queue.begin();
        m_queue.erase(m_queue.begin());
        entity->m_vruntime -= m_floor;
        return entity;
    }

    size_t Size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.size();
    }
private:
    mutable std::mutex m_mutex;
    Queue m_queue;
    CfsEntity* m_running{nullptr};
    double m_floor{0};
};
}
