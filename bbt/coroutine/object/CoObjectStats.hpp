/**
 * @file CoObjectStats.hpp
 * @brief 对象实际创建/析构的非 owning 统计；关闭不改变存活数。
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <string_view>

namespace bbt::coroutine {

/**
 * @brief 某个 kind 的对象计数快照。
 *
 * 四个字段取自同一临界区，所以每一个快照都自洽，不只是「稳定期」：
 * `created_total - destroyed_total == alive` 且 `peak_alive >= alive`。
 * `Close`/`Stop`（若提供）与句柄移动都不改变这些数：只有实际析构会推进
 * `destroyed_total`。
 */
struct CoObjectStats {
    /**
     * 成功创建累计数（含已析构对象）
     */
    std::uint64_t created_total{0};

    /**
     * 实际析构累计数
     */
    std::uint64_t destroyed_total{0};

    /**
     * 当前存活数：created_total - destroyed_total
     */
    std::uint64_t alive{0};

    /**
     * 历史峰值存活数
     */
    std::uint64_t peak_alive{0};
};

/**
 * @brief 按 kind 查询对象计数快照。
 *
 * @param kind 对象 kind，例如 `infra.rpc.Server`。
 * @return 该 kind 的一致快照；未知 kind 返回全零，不分配内存、不建桶。
 */
CoObjectStats GetCoObjectStats(std::string_view kind) noexcept;

namespace detail {

/**
 * @brief 单个 kind 或单个 Manager 的计数累加器。
 *
 * 计数用普通整数加一把短临界区锁，而不是四个独立 atomic：一次创建要同时
 * 推进 created/alive/peak，一次析构要同时推进 destroyed/alive；拆成四个
 * atomic 时读者会看到「alive 已加、created 未加」的中间态，口径就不自洽了。
 * 锁由累加器自己持有，调用方（创建提交、成员析构、查询入口）不必拼装任何
 * 锁协议。
 *
 * 临界区只做整数运算，不调用用户代码或分配内存；析构记账不依赖静态
 * 析构顺序，也不会发起协程等待。
 */
class CoObjectCounters {
public:
    /**
     * @brief 在同一临界区提交创建计数，并更新当前存活数与历史峰值。
     */
    void OnCreate() noexcept;

    /**
     * @brief 结算一次实际析构并更新当前存活数。
     */
    void OnDestroy() noexcept;

    /**
     * @brief 在同一临界区内复制四个计数。
     */
    CoObjectStats GetStats() const noexcept;

private:
    mutable std::mutex m_mutex;
    std::uint64_t m_created{0};
    std::uint64_t m_destroyed{0};
    std::uint64_t m_alive{0};
    std::uint64_t m_peak{0};
};

/**
 * @brief 取该 kind 的计数累加器；首次创建时分配，发布后保持进程寿命。
 */
CoObjectCounters& GetOrCreateCoObjectCounters(std::string_view kind);

}
}
