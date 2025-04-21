#include <bbt/coroutine/detail/GlobalConfig.hpp>
#include <thread>

namespace bbt::coroutine::detail
{



GlobalConfig::GlobalConfig()
{
    if (m_cfg_static_thread_num <= 0)
        m_cfg_static_thread_num = std::thread::hardware_concurrency() > 0 ? std::thread::hardware_concurrency() : 1;
}

} // namespace bbt::coroutine::detail