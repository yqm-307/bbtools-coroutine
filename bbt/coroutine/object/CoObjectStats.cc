#include <bbt/coroutine/object/CoObjectStats.hpp>

#include <mutex>
#include <string>

namespace bbt::coroutine {
namespace {

struct KindCounters {
    explicit KindCounters(std::string_view value) : kind(value) {}

    const std::string kind;
    detail::CoObjectCounters counters;
    KindCounters* next{nullptr};
};

std::atomic<KindCounters*> g_kindCounters{nullptr};
std::mutex g_kindMutex;

KindCounters* FindKind(std::string_view kind) noexcept {
    // ponytail: 面向少量稳定 kind 线性查询；桶数量成为瓶颈时改不可变索引，不引入成员所有权。
    auto bucket = g_kindCounters.load(std::memory_order_acquire);
    while (bucket) {
        if (std::string_view(bucket->kind) == kind)
            return bucket;
        bucket = bucket->next;
    }
    return nullptr;
}

} // namespace

CoObjectStats GetCoObjectStats(std::string_view kind) noexcept {
    if (const auto bucket = FindKind(kind))
        return bucket->counters.GetStats();
    return {};
}

namespace detail {

CoObjectCounters& GetOrCreateCoObjectCounters(std::string_view kind) {
    std::lock_guard<std::mutex> lock(g_kindMutex);
    if (auto bucket = FindKind(kind))
        return bucket->counters;

    // 桶不拥有对象，发布后不回收；native 析构和退出期查询不依赖注册表析构顺序。
    auto bucket = new KindCounters(kind);
    bucket->next = g_kindCounters.load(std::memory_order_relaxed);
    g_kindCounters.store(bucket, std::memory_order_release);
    return bucket->counters;
}

} // namespace detail
} // namespace bbt::coroutine
