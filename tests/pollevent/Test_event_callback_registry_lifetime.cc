/* runtime 与仍存活的 Event 可跨过 main 返回；回调注册表不能提前析构。
 * exit_live_event 留存一个活 Event，以捕获对象的析构退出码观察此边界。
 * normal_reclaim / cancel_reclaim 同时验证正常销毁与取消仍逐项回收回调。
 * 每种模式由 CTest 启动独立进程，退出哨兵用 _exit 避免递归静态析构。
 */
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unistd.h>

#include <bbt/pollevent/Event.hpp>
#include <bbt/pollevent/detail/EventBase.hpp>

#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "FAIL line=%d %s\n", __LINE__, #x); return 1; } } while (0)

namespace
{

constexpr int kExitCodeStaticDtorHit = 42;

struct Sentinel
{
    ~Sentinel()
    {
        m_destructorCount.fetch_add(1, std::memory_order_acq_rel);
        if (m_mainReturned.load(std::memory_order_acquire))
            ::_exit(kExitCodeStaticDtorHit);
    }
    static std::atomic<int>  m_destructorCount;
    static std::atomic<bool> m_mainReturned;
};
std::atomic<int>  Sentinel::m_destructorCount{0};
std::atomic<bool> Sentinel::m_mainReturned{false};

bbt::pollevent::detail::EventBase* GetEventBase()
{
    static auto* base = new bbt::pollevent::detail::EventBase();
    return base;
}

int RunExitLiveEvent()
{
    namespace pe = bbt::pollevent;

    auto* base = GetEventBase();
    CHECK(base != nullptr);

    int fds[2];
    CHECK(::pipe(fds) == 0);

    auto* ev = new pe::Event(
        base, fds[0],
        static_cast<short>(pe::EventOpt::READABLE),
        [sentinel = std::make_shared<Sentinel>()](int, short, pe::EventId) {});
    (void)ev;

    CHECK(pe::Event::CallbackEntryCount() >= 1);

    Sentinel::m_mainReturned.store(true, std::memory_order_release);
    return 0;
}

int RunNormalReclaim()
{
    namespace pe = bbt::pollevent;

    auto* base = GetEventBase();
    CHECK(base != nullptr);

    int fds[2];
    CHECK(::pipe(fds) == 0);

    const auto cb_before = pe::Event::CallbackEntryCount();
    const int  dtor_before = Sentinel::m_destructorCount.load();

    std::weak_ptr<Sentinel> wp;
    {
        auto sentinel = std::make_shared<Sentinel>();
        wp = sentinel;
        CHECK(sentinel.use_count() == 1);
        {
            pe::Event ev(base, fds[0],
                         static_cast<short>(pe::EventOpt::READABLE),
                         [sentinel](int, short, pe::EventId) {});
            CHECK(pe::Event::CallbackEntryCount() == cb_before + 1);
            CHECK(sentinel.use_count() == 2);
        }

        CHECK(pe::Event::CallbackEntryCount() == cb_before);
        CHECK(sentinel.use_count() == 1);

        CHECK(Sentinel::m_destructorCount.load() == dtor_before);
    }

    CHECK(wp.expired());
    CHECK(Sentinel::m_destructorCount.load() == dtor_before + 1);

    ::close(fds[0]);
    ::close(fds[1]);
    return 0;
}

int RunCancelReclaim()
{
    namespace pe = bbt::pollevent;

    auto* base = GetEventBase();
    CHECK(base != nullptr);

    int fds[2];
    CHECK(::pipe(fds) == 0);

    const auto cb_before = pe::Event::CallbackEntryCount();
    const int  dtor_before = Sentinel::m_destructorCount.load();

    std::weak_ptr<Sentinel> wp;
    {
        auto sentinel = std::make_shared<Sentinel>();
        wp = sentinel;
        {
            pe::Event ev(base, fds[0],
                         static_cast<short>(pe::EventOpt::READABLE),
                         [sentinel](int, short, pe::EventId) {});
            CHECK(pe::Event::CallbackEntryCount() == cb_before + 1);

            CHECK(ev.CancelListen() == 0);
            CHECK(pe::Event::CallbackEntryCount() == cb_before);
            CHECK(sentinel.use_count() == 1);
            CHECK(Sentinel::m_destructorCount.load() == dtor_before);
        }

        CHECK(pe::Event::CallbackEntryCount() == cb_before);
        CHECK(Sentinel::m_destructorCount.load() == dtor_before);
    }
    CHECK(wp.expired());
    CHECK(Sentinel::m_destructorCount.load() == dtor_before + 1);

    ::close(fds[0]);
    ::close(fds[1]);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    int rc = 1;
    if (!std::strcmp(argv[1], "exit_live_event"))
        rc = RunExitLiveEvent();
    else if (!std::strcmp(argv[1], "normal_reclaim"))
        rc = RunNormalReclaim();
    else if (!std::strcmp(argv[1], "cancel_reclaim"))
        rc = RunCancelReclaim();
    else
        std::fprintf(stderr, "FAIL unknown mode=%s\n", argv[1]);

    if (rc == 0)
        std::fprintf(stderr, "PASS mode=%s\n", argv[1]);
    return rc;
}
