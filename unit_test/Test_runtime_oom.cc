// 单次分配失败：每个 CTest 模式独立进程；失败退出不冒充恢复。
#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/Hook.hpp>
#include <bbt/pollevent/Event.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <fcntl.h>
#include <unistd.h>
#include <thread>
static thread_local bool fail_next = false;
static thread_local int hits = 0;
void* operator new(std::size_t n) {
    if (fail_next) { fail_next = false; ++hits; throw std::bad_alloc(); }
    if (auto p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
namespace d = bbt::coroutine::detail;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d %s hits=%d\n", __LINE__, #x, hits); return 1; } } while(0)
int main(int argc, char** argv) {
    CHECK(argc == 2);
    const char* mode = argv[1];
    if (!std::strcmp(mode, "defer")) {
        d::CoPoller poller;
        auto event = poller.CreateEvent(-1, bbt::pollevent::EventOpt::TIMEOUT, [](int, short, int64_t){});
        std::weak_ptr<bbt::pollevent::Event> weak = event;
        fail_next = true;
        try { poller.DeferDestroyEvent(std::move(event)); } catch (...) { fail_next = false; CHECK(false); }
        fail_next = false;
        CHECK(hits == 1); CHECK(!weak.expired());
        std::thread other([&]{ poller.FlushDeferredEvents(); }); other.join();
        CHECK(!weak.expired());
        poller.PollOnce(); CHECK(weak.expired());
    } else if (!std::strcmp(mode, "trigger")) {
        int count = 0; int flags = 0;
        auto event = d::CoPollEvent::Create(1, [&](auto, int f, int){ ++count; flags=f; });
        CHECK(event->InitFdEvent(-1, d::POLL_EVENT_TIMEOUT, 50) == 0);
        CHECK(event->Regist() == 0); CHECK(!event->CommitPark());
        fail_next = true;
        CHECK(event->Trigger(d::POLL_EVENT_CUSTOM) == 0);
        fail_next = false;
        CHECK(hits == 1); CHECK(event->IsFinal()); CHECK(count == 1); CHECK(flags == d::POLL_EVENT_CUSTOM);
        CHECK(event->Trigger(d::POLL_EVENT_CANCELLED) == -1);
        CHECK(event->Trigger(d::POLL_EVENT_CUSTOM) == -1);
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        d::CoPoller::GetInstance()->PollOnce();
        CHECK(count == 1); CHECK(event->IsFinal());
    } else {
        int fd[2]; CHECK(pipe2(fd, O_NONBLOCK) == 0);
        CHECK(d::Hook_Close(fd[1]) == 0); // 初始化全局表；目标 fd 尚无槽位
        if (!std::strcmp(mode, "fresh")) {
            fail_next = true; int rc = -99;
            try { rc = d::Hook_Close(fd[0]); } catch (...) { fail_next=false; CHECK(false); }
            fail_next = false; CHECK(hits == 1); CHECK(rc == 0);
        } else {
            int count = 0;
            auto event = d::CoPollEvent::Create(1, [&](auto, int f, int){ if (f & d::POLL_EVENT_CLOSED) ++count; });
            CHECK(event->InitFdEvent(fd[0], bbt::pollevent::EventOpt::READABLE, 0) == 0);
            CHECK(event->Regist() == 0); CHECK(!event->CommitPark());
            CHECK(d::CoPollEvent::FdWaiterCount(fd[0]) == 1);
            auto epoch = d::CoPollEvent::FdEpoch(fd[0]);
            if (!std::strcmp(mode, "prepare")) {
                bool threw = false; fail_next = true;
                try { d::CoPollEvent::BeginFdClose(fd[0]); } catch (const std::bad_alloc&) { threw = true; }
                fail_next = false; CHECK(hits == 1); CHECK(threw);
                CHECK(!d::CoPollEvent::FdIsClosing(fd[0]));
                CHECK(d::CoPollEvent::FdEpoch(fd[0]) == epoch);
                CHECK(d::CoPollEvent::FdWaiterCount(fd[0]) == 1); CHECK(count == 0);
                CHECK(fcntl(fd[0], F_GETFD) >= 0);
            } else { fail_next = true; }
            int rc = -99;
            try { rc = d::Hook_Close(fd[0]); } catch (...) { fail_next = false; CHECK(false); }
            fail_next = false;
            CHECK(hits == 1); CHECK(rc == 0); CHECK(count == 1); CHECK(event->IsFinal());
            CHECK(d::CoPollEvent::FdEpoch(fd[0]) == epoch + 1);
            d::CoPoller::GetInstance()->PollOnce(); CHECK(count == 1);
        }
        CHECK(!d::CoPollEvent::FdIsClosing(fd[0]));
        errno=0; CHECK(fcntl(fd[0], F_GETFD) == -1); CHECK(errno == EBADF);
    }
    std::fprintf(stderr, "PASS mode=%s hits=%d settled=1\n", mode, hits);
    return 0;
}
