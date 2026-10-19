#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <chrono>
#include <limits>
#include <thread>

namespace bbt::coroutine::detail
{



GlobalConfig::GlobalConfig()
{
    if (m_cfg_static_thread_num <= 0)
        m_cfg_static_thread_num = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 1;
}

bool GlobalConfig::ResolveProcesserScalingBounds(ProcesserScalingBounds& out_bounds) const noexcept
{
    const size_t initial = m_cfg_static_thread_num;
    const size_t min_num = m_cfg_processer_min_num == 0 ? initial : m_cfg_processer_min_num;
    const size_t max_num = m_cfg_processer_max_num == 0 ? initial : m_cfg_processer_max_num;
    /* 现有启动 latch 与 worker 创建循环使用 int，必须在窄化前拒绝超大数量。 */
    const size_t max_count = static_cast<size_t>(std::numeric_limits<int>::max());
    if (min_num == 0 || min_num > max_num || max_num > max_count ||
        initial < min_num || initial > max_num)
        return false;

    if (m_cfg_processer_scale_interval_ms == 0 ||
        static_cast<uintmax_t>(m_cfg_processer_scale_interval_ms) >
            static_cast<uintmax_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max()) ||
        m_cfg_processer_scale_up_samples == 0 || m_cfg_processer_scale_down_samples == 0 ||
        m_cfg_processer_scale_step == 0 || m_cfg_processer_scale_step > max_count)
        return false;

    if (m_cfg_processer_scale_up_busy_percent > 100 ||
        m_cfg_processer_scale_down_busy_percent > m_cfg_processer_scale_up_busy_percent ||
        m_cfg_processer_scale_up_queue_watermark == 0 ||
        m_cfg_processer_scale_down_queue_watermark > m_cfg_processer_scale_up_queue_watermark)
        return false;

    out_bounds = ProcesserScalingBounds{initial, min_num, max_num};
    return true;
}

} // namespace bbt::coroutine::detail