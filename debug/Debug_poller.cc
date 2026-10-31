#include <bbt/core/clock/Clock.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Processer.hpp>

using namespace bbt::coroutine::detail;

int main()
{
    auto co_sptr = Coroutine::Create(4096, [](){});
    auto event = CoPollEvent::Create(co_sptr->GetId(),[co_sptr](auto, int, int){
        printf("timeout [%ld]\n", bbt::core::clock::now<>().time_since_epoch().count());
    });

    printf("regist [%ld]\n", bbt::core::clock::now<>().time_since_epoch().count());
    Assert(event->InitFdEvent(-1, bbt::pollevent::EventOpt::TIMEOUT, 1000) == 0);
    Assert(event->Regist() == 0);

    auto end_ts = bbt::core::clock::nowAfter(bbt::core::clock::seconds(2));
    while (!bbt::core::clock::is_expired<bbt::core::clock::seconds>(end_ts))
    {
        CoPoller::GetInstance()->PollOnce();
    }
}