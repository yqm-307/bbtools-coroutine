#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>

using namespace bbt::coroutine::detail;

int main()
{
    auto co_sptr = Coroutine::Create(4096, [](){});
    auto event = CoPollEvent::Create(co_sptr, 1000);

    printf("[%ld]\n", bbt::clock::now<>().time_since_epoch().count());
    Assert(event->RegistEvent() == 0);


    for (int i = 0; i < 100; i++)
    {
        CoPoller::GetInstance()->PollOnce();
    }
}