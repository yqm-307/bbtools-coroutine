/* 单个 Processer 的 CFS 就绪队列：节点内嵌在 Coroutine 里，队列只负责排序、记账和运行权移交。 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <boost/intrusive/set.hpp>

namespace bbt::coroutine::detail {
class Coroutine;
class CfsRunQueue;

/* 队列节点：每个 Coroutine 内嵌一个（m_cfs），随 Coroutine 生灭，不持有 Processer 指针。
 * 字段由所属 CfsRunQueue 在锁内写入。vruntime 是相对源队列 floor 的债务：Enqueue 按目标 floor
 * 重挂、Take 时折算回债务，跨队列迁移因此只带走债务，不带源队列的绝对服务量。 */
class CfsEntity : public boost::intrusive::set_base_hook<> {
public:
    CfsEntity() = default;
    CfsEntity(const CfsEntity&) = delete;
    CfsEntity& operator=(const CfsEntity&) = delete;
    uint64_t ServiceUs() const noexcept
    {
        return m_serviceUs;
    }
    Coroutine* Task() const noexcept
    {
        return m_task;
    }
    int Priority() const noexcept
    {
        return m_priority;
    }

private:
    friend class CfsRunQueue;
    Coroutine* m_task{nullptr};
    uint64_t m_serviceUs{0};
    double m_vruntime{0};
    uint64_t m_weight{1};
    int m_priority{0};
    bool m_waiting{false};
};

/* CFS 队列边界：只做排序、记账和运行权移交，不 Resume/Wake、不分配节点（被选中的协程由
 * Processer 恢复）。集合、floor 与 m_running 受同一把 m_mutex 保护；m_running 置位期间再 Pick
 * 返回 nullptr，Pick 置位、Account 清位。 */
class CfsRunQueue {
    struct Earlier {
        bool operator()(const CfsEntity& a, const CfsEntity& b) const noexcept
        {
            return a.m_vruntime < b.m_vruntime;
        }
    };
    using Queue = boost::intrusive::multiset<CfsEntity, boost::intrusive::compare<Earlier>>;

public:
    // 入队并把 vruntime 重挂到本队列 floor。已链在队列里或正在运行时返回 false，节点不被修改。
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

    // 取 vruntime 最小的节点并标记为运行中；已有运行者或队列为空时返回 nullptr。
    CfsEntity* Pick()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running != nullptr || m_queue.empty())
            return nullptr;
        m_running = &*m_queue.begin();
        m_queue.erase(m_queue.begin());
        return m_running;
    }

    // 结清一次运行：elapsed_us 记服务量，按 weight 摊进 vruntime，然后释放运行权。
    void Account(CfsEntity& entity, uint64_t elapsed_us, bool waiting)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running != &entity)
            std::terminate(); // 内部唯一执行者协议损坏，不能带坏账继续发布。
        const auto charge = std::max<uint64_t>(1, elapsed_us);
        entity.m_serviceUs += std::min(charge, std::numeric_limits<uint64_t>::max() - entity.m_serviceUs);
        entity.m_vruntime += static_cast<double>(charge) / entity.m_weight;
        const auto progress =
            m_queue.empty() ? entity.m_vruntime : std::min(entity.m_vruntime, m_queue.begin()->m_vruntime);
        m_floor = std::max(m_floor, progress);
        entity.m_vruntime -= m_floor;
        entity.m_waiting = waiting;
        // CommitPark 可能立即发布甚至销毁 Coroutine；不保留运行对象。
        m_running = nullptr;
    }

    // steal/retire 用：导出可运行节点，不动 m_running。源锁在函数内释放，目标锁由调用方稍后取得。
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

    // 只数就绪节点，不含正在运行的那个。
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
