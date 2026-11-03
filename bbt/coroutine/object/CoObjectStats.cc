#include <bbt/coroutine/object/CoObjectStats.hpp>

#include <atomic>
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

// 桶表只在创建路径追加，发布后不再删除：单链表 + acquire/release 发布，
// 查询侧无锁遍历。桶数量是进程内稳定的小集合（按 kind），不值得为它引入
// 索引结构或桶回收。
std::atomic<KindCounters*> g_kindCounters{nullptr};
std::mutex g_kindMutex;

KindCounters* FindKind(std::string_view kind) noexcept {
    // ponytail: 线性查询面向少量稳定 kind；桶数量成为瓶颈时改不可变索引，
    // 升级前不需要成员所有权或回收语义。
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
    // 未知 kind 直接返回全零：查询路径不建桶、不分配，因此可以在
    // noexcept 入口和退出期安全调用。
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
