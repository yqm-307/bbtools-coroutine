#include <atomic>
#include <limits>
#include <stdexcept>
#include <utility>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/object/CoObject.hpp>

namespace bbt::coroutine
{

namespace
{

std::atomic<CoObjectId> g_next_object_id{1};

}

RuntimeGeneration CurrentRuntimeGeneration() noexcept
{
    return static_cast<RuntimeGeneration>(
        detail::Scheduler::GetInstance()->GetRunGeneration());
}

CoObjectInfo CreateObjectInfo(std::string kind, std::string name)
{
    const auto generation = CurrentRuntimeGeneration();
    if (generation == 0)
        throw std::logic_error{"CoObject: runtime generation unavailable"};

    auto cur = g_next_object_id.load(std::memory_order_relaxed);
    for (;;)
    {
        if (cur == std::numeric_limits<CoObjectId>::max())
            throw std::overflow_error{"CoObject: object id exhausted"};
        if (g_next_object_id.compare_exchange_weak(
                cur, cur + 1,
                std::memory_order_relaxed, std::memory_order_relaxed))
            break;
    }

    return CoObjectInfo{cur, generation, std::move(kind), std::move(name)};
}

} // namespace bbt::coroutine
