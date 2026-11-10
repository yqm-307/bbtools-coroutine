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

void GlobalConfig::FreezeRuntimeSetup() noexcept
{
    /* Start is process-lifetime; a repeated Start must not recapture fields
     * that a caller changed after the first snapshot was published. */
    if (m_runtime_setup_frozen.load(std::memory_order_acquire))
        return;

    m_runtime_snapshot.max_coroutine = m_cfg_max_coroutine;
    m_runtime_snapshot.static_coroutine = m_cfg_static_coroutine;
    m_runtime_snapshot.stack_size = m_cfg_stack_size;
    m_runtime_snapshot.stack_protect = m_cfg_stack_protect;
    m_runtime_snapshot.scan_interval_ms = m_cfg_scan_interval_ms;
    m_runtime_snapshot.profile_printf_ms = m_cfg_profile_printf_ms;
    m_runtime_snapshot.processer_auto_scale = m_cfg_processer_auto_scale;
    m_runtime_snapshot.processer_scale_interval_ms = m_cfg_processer_scale_interval_ms;
    m_runtime_snapshot.processer_scale_up_samples = m_cfg_processer_scale_up_samples;
    m_runtime_snapshot.processer_scale_down_samples = m_cfg_processer_scale_down_samples;
    m_runtime_snapshot.processer_scale_step = m_cfg_processer_scale_step;
    m_runtime_snapshot.processer_scale_up_busy_percent = m_cfg_processer_scale_up_busy_percent;
    m_runtime_snapshot.processer_scale_down_busy_percent = m_cfg_processer_scale_down_busy_percent;
    m_runtime_snapshot.processer_scale_up_queue_watermark = m_cfg_processer_scale_up_queue_watermark;
    m_runtime_snapshot.processer_scale_down_queue_watermark = m_cfg_processer_scale_down_queue_watermark;
    m_runtime_snapshot.processer_get_co_from_g_count = m_cfg_processer_get_co_from_g_count;
    m_runtime_snapshot.processer_steal_once_min_task_num = m_cfg_processer_steal_once_min_task_num;
    m_runtime_snapshot.processer_worksteal_timeout_ms = m_cfg_processer_worksteal_timeout_ms;
    m_runtime_snapshot.processer_proc_interval_us = m_cfg_processer_proc_interval_us;
    m_runtime_snapshot.processer_priority_runtime_budget_us = m_cfg_processer_priority_runtime_budget_us;
    m_runtime_snapshot.stackpool_min_alloc_size = m_cfg_stackpool_min_alloc_size;
    m_runtime_snapshot.stackpool_sample_interval = m_cfg_stackpool_sample_interval;
    m_runtime_snapshot.stackpool_adjust_interval = m_cfg_stackpool_adjust_interval;
    m_runtime_snapshot.coevent_exception_callback = m_ext_coevent_exception_callback;
    m_runtime_snapshot.worker_stall_warn_ms = m_cfg_worker_stall_warn_ms;
    m_runtime_snapshot.worker_stall_callback = m_ext_worker_stall_callback;
    m_runtime_setup_frozen.store(true, std::memory_order_release);
}

bool GlobalConfig::IsRuntimeSetupFrozen() const noexcept
{
    return m_runtime_setup_frozen.load(std::memory_order_acquire);
}

const RuntimeConfigSnapshot& GlobalConfig::RuntimeSnapshot() const noexcept
{
    return m_runtime_snapshot;
}

} // namespace bbt::coroutine::detail