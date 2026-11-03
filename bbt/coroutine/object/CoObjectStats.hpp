/* 对象实际创建/析构的非 owning 统计；关闭不改变存活数。 */
#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

namespace bbt::coroutine {

struct CoObjectStats {
    std::uint64_t created_total{0};
    std::uint64_t destroyed_total{0};
    std::uint64_t alive{0};
    std::uint64_t peak_alive{0};
};

/* 未知 kind 返回全零；并发跨字段读取不承诺原子一致，查询不分配内存。 */
CoObjectStats GetCoObjectStats(std::string_view kind) noexcept;

namespace detail {

class CoObjectCounters {
public:
    void OnCreate() noexcept {
        m_created.fetch_add(1, std::memory_order_relaxed);
        const auto alive = m_alive.fetch_add(1, std::memory_order_relaxed) + 1;
        auto peak = m_peak.load(std::memory_order_relaxed);
        while (peak < alive && !m_peak.compare_exchange_weak(peak, alive, std::memory_order_relaxed)) {}
    }

    void OnDestroy() noexcept {
        m_destroyed.fetch_add(1, std::memory_order_relaxed);
        m_alive.fetch_sub(1, std::memory_order_relaxed);
    }

    CoObjectStats GetStats() const noexcept {
        return {m_created.load(std::memory_order_relaxed), m_destroyed.load(std::memory_order_relaxed),
                m_alive.load(std::memory_order_relaxed), m_peak.load(std::memory_order_relaxed)};
    }

private:
    std::atomic<std::uint64_t> m_created{0};
    std::atomic<std::uint64_t> m_destroyed{0};
    std::atomic<std::uint64_t> m_alive{0};
    std::atomic<std::uint64_t> m_peak{0};
};

/* 类型桶只在创建路径分配，发布后保持进程寿命，不依赖静态析构顺序。 */
CoObjectCounters& GetOrCreateCoObjectCounters(std::string_view kind);

} // namespace detail
} // namespace bbt::coroutine
