#include <bbt/base/clock/Clock.hpp>
#include <bbt/coroutine/detail/Coroutine.hpp>
#include <bbt/coroutine/detail/CoPoller.hpp>
#include <bbt/coroutine/detail/CoPollEvent.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/Processer.hpp>
#include <bbt/coroutine/detail/CoEventBase.hpp>
#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine::detail;
using namespace bbt::coroutine;

class TestEvent:
    public bbt::coroutine::detail::CoEventBase,
    public std::enable_shared_from_this<TestEvent>
{
public:
    friend class bbt::coroutine::detail::CoPoller;

    static std::shared_ptr<TestEvent> Create() { return std::make_shared<TestEvent>(); }


    bool IsTrigger() { return m_triggered; }

    int Timeout(int ms, std::function<void()> cb)
    {
        auto [ret, event_id] = g_bbt_poller->Regist<TestEvent>(std::dynamic_pointer_cast<IPollEvent>(shared_from_this()), ms);
        Assert(ret == 0);
        if (ret != 0)
            return -1;
        
        m_id = event_id;
        m_handle = cb;
        return 0;
    }

    int UnRegist() {
        return g_bbt_poller->UnRegist(m_id);
    }

protected:
    virtual int OnNotify(short trigger_events, int customkey) override
    {
        auto co = GetWaitCo();
        co->Active();
        m_triggered = true;
        m_handle();
        return 0;
    }

private:
    bool m_triggered{false};
    CoPollEventId m_id{0};
    std::function<void()> m_handle;
};

int main()
{
    g_scheduler->Start(true);

    bbt::thread::CountDownLatch l{1};

    std::atomic_int count = 0;
    const int timeout_ms = 200;

    bbtco [&](){
        auto event = TestEvent::Create();

        Assert(event->Timeout(timeout_ms, [&](){ count = 1; }) == 0);

        while (count != 1)
        {
            sleep(1);
        }
        Assert(count == 1);
        l.Down();
    };

    l.Wait();
    g_scheduler->Stop();
}