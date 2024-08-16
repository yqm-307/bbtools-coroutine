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
        printf("timeout [%ld]\n", bbt::clock::now<>().time_since_epoch().count());
    });

    printf("regist [%ld]\n", bbt::clock::now<>().time_since_epoch().count());
    Assert(event->InitFdEvent(-1, bbt::pollevent::EventOpt::TIMEOUT, 1000) == 0);
    Assert(event->Regist() == 0);

    auto end_ts = bbt::clock::nowAfter(bbt::clock::seconds(2));
    while (!bbt::clock::is_expired<bbt::clock::seconds>(end_ts))
    {
        CoPoller::GetInstance()->PollOnce();
    }
}