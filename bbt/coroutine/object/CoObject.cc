#include <atomic>
#include <limits>
#include <stdexcept>
#include <utility>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/object/CoObject.hpp>

namespace bbt::coroutine {

namespace {

std::atomic<CoObjectId> g_next_object_id{1};

}

CoObject::CoObject(std::string kind, std::string name) : MemberBase(CreateObjectInfo(std::move(kind), std::move(name)))
{
}

std::shared_ptr<CoObjectManager> GetCoObjectManager()
{
    static const auto manager = new std::shared_ptr<CoObjectManager>(std::make_shared<CoObjectManager>());
    return *manager;
}

CoObjectInfo CreateObjectInfo(std::string kind, std::string name)
{
    /* 前置条件是「运行时已初始化」：运行时不设停机/重启，身份一旦可分配就
     * 永远有效，因此不需要再绑定某个代际。 */
    if (!detail::Scheduler::GetInstance()->IsInitialized())
        throw std::logic_error{"CoObject: runtime not initialized"};

    auto cur = g_next_object_id.load(std::memory_order_relaxed);
    for (;;) {
        if (cur == std::numeric_limits<CoObjectId>::max())
            throw std::overflow_error{"CoObject: object id exhausted"};
        if (g_next_object_id.compare_exchange_weak(cur, cur + 1, std::memory_order_relaxed, std::memory_order_relaxed))
            break;
    }

    return CoObjectInfo{cur, std::move(kind), std::move(name)};
}

} // namespace bbt::coroutine
