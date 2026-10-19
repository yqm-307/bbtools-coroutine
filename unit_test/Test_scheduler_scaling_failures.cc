// 每个分配序号 exec 独立进程；仅 arm 当前线程的一次 new，不污染 worker。
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#include <thread>
#if defined(__linux__)
#include <dlfcn.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static thread_local long fail_after = -1;
static thread_local int allocation_hits = 0;
static std::atomic_int worker_hits{0};
static thread_local bool worker_armed = false;
void* operator new(std::size_t n)
{
    if (fail_after == 0) {
        fail_after = -1;
        ++allocation_hits;
        if (worker_armed) ++worker_hits;
        throw std::bad_alloc();
    }
    if (fail_after > 0) --fail_after;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

#if defined(__linux__)
static thread_local bool count_creates = false;
static thread_local int create_calls = 0;
static thread_local int fail_create = 0;
static thread_local bool fail_worker_tls = false;
struct ThreadEntry { void* (*entry)(void*); void* arg; };
static void* ArmedEntry(void* p)
{
    const auto entry = *static_cast<ThreadEntry*>(p);
    std::free(p);
    worker_armed = true;
    fail_after = 0;
    return entry.entry(entry.arg);
}
extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                               void* (*entry)(void*), void* arg) noexcept
{
    using Create = int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
    static auto real_create = reinterpret_cast<Create>(dlsym(RTLD_NEXT, "pthread_create"));
    if (!real_create) std::abort();
    if (count_creates) {
        ++create_calls;
        if (create_calls == fail_create) return EAGAIN;
        if (fail_worker_tls && create_calls == 2) {
            auto* state = static_cast<ThreadEntry*>(std::malloc(sizeof(ThreadEntry)));
            if (!state) return ENOMEM;
            *state = {entry, arg};
            const int rc = real_create(thread, attr, ArmedEntry, state);
            if (rc) std::free(state);
            return rc;
        }
    }
    return real_create(thread, attr, entry, arg);
}
#endif

using namespace bbt::coroutine;
using namespace bbt::coroutine::detail;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d %s\n", __LINE__, #x); return 1; } } while (0)
static size_t ThreadCount()
{
    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task")) {
        (void)entry;
        ++count;
    }
    return count;
}
static int Child(const char* mode, long nth)
{
    const bool shrink = !std::strcmp(mode, "shrink");
    auto& cfg = *g_bbt_coroutine_config;
    cfg.m_cfg_static_thread_num = shrink ? 3 : 1;
    cfg.m_cfg_processer_min_num = 1;
    cfg.m_cfg_processer_max_num = 3;
    cfg.m_cfg_processer_auto_scale = true;
    cfg.m_cfg_stack_protect = false;
    cfg.m_cfg_worker_stall_warn_ms = 0;
    g_scheduler->Start(SCHE_START_OPT_SCHE_NO_LOOP);
    const auto old_ids = g_scheduler->GetWorkerProcesserIds();
    const size_t old_threads = ThreadCount();
    const size_t old_count = old_ids.size();
    const size_t target = shrink ? 1 : 3;
    const bool alloc = shrink || !std::strcmp(mode, "expand");
    count_creates = true;
    fail_create = !std::strcmp(mode, "pthread") ? 2 : 0;
    fail_worker_tls = !std::strcmp(mode, "tls_alloc");
    fail_after = alloc ? nth : -1;
    bool threw = false;
    auto result = ProcesserResizeResult::Ok;
    try { result = g_scheduler->SetProcesserNum(target); }
    catch (...) { threw = true; }
    fail_after = -1;
    count_creates = false;
    CHECK(!threw);
    if (alloc && allocation_hits == 0) {
        CHECK(result == (shrink ? ProcesserResizeResult::Pending : ProcesserResizeResult::Ok));
        CHECK(g_scheduler->GetProcesserNum() == target);
        CHECK(g_scheduler->GetProcesserTargetNum() == target);
        CHECK(g_scheduler->GetProcesserScalingMode() == ProcesserScalingMode::Manual);
        std::printf("COMPLETE mode=%s first_uninjected=%ld creates=%d\n", mode, nth, create_calls);
        return 42; // 只作扫描结束证据，不计为注入覆盖。
    }
    CHECK(alloc ? allocation_hits == 1 : (!std::strcmp(mode, "pthread") ? create_calls == 2 : worker_hits == 1));
    std::printf("RESULT mode=%s nth=%ld hits=%d result=%d active=%zu target=%zu mode_value=%d creates=%d worker_hits=%d\n",
        mode, nth, allocation_hits, static_cast<int>(result), g_scheduler->GetProcesserNum(),
        g_scheduler->GetProcesserTargetNum(), static_cast<int>(g_scheduler->GetProcesserScalingMode()),
        create_calls, worker_hits.load());
#ifdef BBT_COROUTINE_PROFILE
    std::string profile;
    g_bbt_profiler->ProfileInfo(profile);
    size_t rows = 0;
    for (size_t pos = profile.find("\tPID："); pos != std::string::npos; pos = profile.find("\tPID：", pos + 1)) ++rows;
    std::printf("PROFILE rows=%zu active=%zu\n", rows, g_scheduler->GetProcesserNum());
    CHECK(rows == old_count);
#endif
    CHECK(result == (shrink ? ProcesserResizeResult::Busy : ProcesserResizeResult::CreateFailed));
    CHECK(g_scheduler->GetProcesserNum() == old_count);
    CHECK(g_scheduler->GetProcesserTargetNum() == old_count);
    CHECK(g_scheduler->GetProcesserScalingMode() == ProcesserScalingMode::Auto);
    CHECK(g_scheduler->GetWorkerProcesserIds() == old_ids);
    CHECK(g_scheduler->GetProcesserRetiringNum() == 0);
    CHECK(g_scheduler->GetProcesserExitedUnjoinedNum() == 0);
    // 批次取消必须物理 join，不能只隐藏新身份。
    CHECK(ThreadCount() == old_threads);
    CHECK(g_scheduler->SetProcesserNum(target) == (shrink ? ProcesserResizeResult::Pending : ProcesserResizeResult::Ok));
    CHECK(g_scheduler->GetProcesserNum() == target);
    CHECK(g_scheduler->GetProcesserTargetNum() == target);
    CHECK(g_scheduler->GetProcesserScalingMode() == ProcesserScalingMode::Manual);
#ifdef BBT_COROUTINE_PROFILE
    if (!shrink) {
        g_bbt_profiler->ProfileInfo(profile);
        rows = 0;
        for (size_t pos = profile.find("\tPID："); pos != std::string::npos;
             pos = profile.find("\tPID：", pos + 1)) ++rows;
        CHECK(rows == target);
    }
#endif
    std::printf("INJECTED mode=%s nth=%ld creates=%d worker_hits=%d retry=ok\n", mode, nth, create_calls, worker_hits.load());
    return 0;
}
int main(int argc, char** argv)
{
#if defined(__linux__)
    CHECK(argc >= 2);
    if (argc == 4 && !std::strcmp(argv[1], "child")) return Child(argv[2], std::strtol(argv[3], nullptr, 10));
    const char* mode = argv[1];
    const bool sweep = !std::strcmp(mode, "expand") || !std::strcmp(mode, "shrink");
    int injected = 0;
    for (int nth = 0; nth < (sweep ? 256 : 1); ++nth) {
        char number[24];
        std::snprintf(number, sizeof(number), "%d", nth);
        const pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) {
            execl(argv[0], argv[0], "child", mode, number, static_cast<char*>(nullptr));
            _exit(127);
        }
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status));
        const int rc = WEXITSTATUS(status);
        if (rc == 42 && sweep) {
            CHECK(injected > 0);
            std::printf("SWEEP mode=%s injected=%d first_complete=%d\n", mode, injected, nth);
            return 0;
        }
        CHECK(rc == 0);
        ++injected;
        if (!sweep) return 0;
    }
    CHECK(false); // 未见首次完整成功，扫描不得冒充闭环。
#else
    (void)argc; (void)argv;
    return 77;
#endif
}
