#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Processer.hpp>

using namespace bbt::coroutine::detail;

int main()
{
    auto co_sptr = Coroutine::Create(4096, [](){});
    auto event = CoPollEvent::Create(co_sptr,[co_sptr](auto, int, int){
        printf("[%ld] [%ld]\n", bbt::clock::now<>().time_since_epoch().count());
    });

    printf("[%ld]\n", bbt::clock::now<>().time_since_epoch().count());
    Assert(event->InitFdEvent(-1, bbt::pollevent::EventOpt::TIMEOUT, 1000) == 0);
    Assert(event->Regist() == 0);

    for (int i = 0; i < 100; i++)
    {
        CoPoller::GetInstance()->PollOnce();
        std::this_thread::sleep_for(bbt::clock::milliseconds(20));
    }
}