#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <chrono>
#include <limits>
#include <thread>

namespace bbt::coroutine::detail {

GlobalConfig::GlobalConfig()
{

    /**
     * 只解析初始 worker 数，其余字段留给调用方在 Start 前赋值；0 视为未配置。
     */
    if (m_cfg_static_thread_num <= 0)
        m_cfg_static_thread_num = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 1;
}

bool GlobalConfig::ResolveProcesserScalingBounds(ProcesserScalingBounds& out_bounds) const noexcept
{

    /**
     * 三段校验，全部通过才写 out_bounds：数量边界 → 采样/步长 → busy 与队列水位。
     * 只读配置、不启动线程；失败时 out_bounds 保持调用方原值。
     */
    const size_t initial = m_cfg_static_thread_num;
    const size_t min_num = m_cfg_processer_min_num == 0 ? initial : m_cfg_processer_min_num;
    const size_t max_num = m_cfg_processer_max_num == 0 ? initial : m_cfg_processer_max_num;

    /**
     * 现有启动 latch 与 worker 创建循环使用 int，必须在窄化前拒绝超大数量。
     */
    const size_t max_count = static_cast<size_t>(std::numeric_limits<int>::max());
    if (min_num == 0 || min_num > max_num || max_num > max_count || initial < min_num || initial > max_num)
        return false;

    if (m_cfg_processer_scale_interval_ms == 0
        || static_cast<uintmax_t>(m_cfg_processer_scale_interval_ms)
               > static_cast<uintmax_t>(std::numeric_limits<std::chrono::milliseconds::rep>::max())
        || m_cfg_processer_scale_up_samples == 0 || m_cfg_processer_scale_down_samples == 0
        || m_cfg_processer_scale_step == 0 || m_cfg_processer_scale_step > max_count)
        return false;

    if (m_cfg_processer_scale_up_busy_percent > 100
        || m_cfg_processer_scale_down_busy_percent > m_cfg_processer_scale_up_busy_percent
        || m_cfg_processer_scale_up_queue_watermark == 0
        || m_cfg_processer_scale_down_queue_watermark > m_cfg_processer_scale_up_queue_watermark)
        return false;

    out_bounds = ProcesserScalingBounds{initial, min_num, max_num};
    return true;
}

void GlobalConfig::FreezeRuntimeSetup()
{
    std::lock_guard<std::mutex> lock(m_runtime_setup_mutex);
    /**
     * Start is process-lifetime; a repeated Start must not recapture fields
     * that a caller changed after the first snapshot was published.
     */
    if (m_runtime_setup_frozen.load(std::memory_order_acquire))
        return;

    _CaptureSetup(m_runtime_snapshot);
    m_runtime_setup_frozen.store(true, std::memory_order_release);
}

void GlobalConfig::_CaptureSetup(RuntimeConfigSnapshot& out) const
{
    out.max_coroutine = m_cfg_max_coroutine;
    out.static_coroutine = m_cfg_static_coroutine;
    out.stack_size = m_cfg_stack_size;
    out.stack_protect = m_cfg_stack_protect;
    out.scan_interval_ms = m_cfg_scan_interval_ms;
    out.profile_printf_ms = m_cfg_profile_printf_ms;
    out.processer_auto_scale = m_cfg_processer_auto_scale;
    out.processer_scale_interval_ms = m_cfg_processer_scale_interval_ms;
    out.processer_scale_up_samples = m_cfg_processer_scale_up_samples;
    out.processer_scale_down_samples = m_cfg_processer_scale_down_samples;
    out.processer_scale_step = m_cfg_processer_scale_step;
    out.processer_scale_up_busy_percent = m_cfg_processer_scale_up_busy_percent;
    out.processer_scale_down_busy_percent = m_cfg_processer_scale_down_busy_percent;
    out.processer_scale_up_queue_watermark = m_cfg_processer_scale_up_queue_watermark;
    out.processer_scale_down_queue_watermark = m_cfg_processer_scale_down_queue_watermark;
    out.processer_get_co_from_g_count = m_cfg_processer_get_co_from_g_count;
    out.processer_steal_once_min_task_num = m_cfg_processer_steal_once_min_task_num;
    out.processer_worksteal_timeout_ms = m_cfg_processer_worksteal_timeout_ms;
    out.processer_proc_interval_us = m_cfg_processer_proc_interval_us;
    out.processer_priority_runtime_budget_us = m_cfg_processer_priority_runtime_budget_us;
    out.stackpool_min_alloc_size = m_cfg_stackpool_min_alloc_size;
    out.stackpool_sample_interval = m_cfg_stackpool_sample_interval;
    out.stackpool_adjust_interval = m_cfg_stackpool_adjust_interval;
    out.coevent_exception_callback = m_ext_coevent_exception_callback;
    out.worker_stall_warn_ms = m_cfg_worker_stall_warn_ms;
    out.worker_stall_callback = m_ext_worker_stall_callback;
}

bool GlobalConfig::IsRuntimeSetupFrozen() const noexcept
{
    return m_runtime_setup_frozen.load(std::memory_order_acquire);
}

const RuntimeConfigSnapshot& GlobalConfig::RuntimeSnapshot() const
{
    if (m_runtime_setup_frozen.load(std::memory_order_acquire))
        return m_runtime_snapshot;

    /**
     * 冻结前（runtime 未启动的白盒/工厂路径）按当前公共字段取值，未改动的字段即为默认值；
     * 不能返回零初始化的快照，否则栈上限和栈大小都会被当成 0 静默拒绝。每线程一份副本：
     * 冻结前可能有多个线程同时读，不能共写同一个对象；引用只在本线程下次调用前有效。
     */
    thread_local RuntimeConfigSnapshot pending;
    _CaptureSetup(pending);
    return pending;
}

}