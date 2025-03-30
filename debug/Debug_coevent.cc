#include <bbt/coroutine/coroutine.hpp>


int main()
{
    g_scheduler->Start();
    std::atomic_bool hold{false};
    __bbtco_event_regist_ex(-1, EV_PERSIST, 10) [&](int fd, short events)
    {
        Assert(hold == false);
        hold = true;
        return true;
    };
    g_scheduler->Stop();
}