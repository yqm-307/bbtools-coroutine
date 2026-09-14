/**
 * @file Test_stop_no_unwind.cc
 * @brief #338/#339 语义回归测试：Stop 对挂起协程直接销毁、不做栈展开；custom
 *        Wait 族的生命周期（登记/预取消/唤醒/Stop 回收责任链）
 *
 * 契约：agent-docs/2026-09-07-core-runtime-contract.md §6
 * 挂起协程在 Stop 时被直接销毁，栈上对象不执行析构与 RAII。
 * #339：CoWaiter 各 Wait 必须经 Coroutine::_RegistAwaitEvent 公共登记路径
 * （预取消自唤醒 + _TrackParked），Stop 才能回收 custom 等待的协程闭包与栈；
 * DestroyParkedCoroutines 对 UnRegist 失败（唤醒在途）的协程跳过删除，
 * Scheduler::OnActiveCoroutine 在停机或代际失配时直接回收，不把旧协程投递
 * 到新一代队列。
 */

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/sync/CoWaiter.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/detail/LocalThread.hpp>
#include <bbt/coroutine/detail/GlobalConfig.hpp>

using namespace bbt::coroutine;

BOOST_AUTO_TEST_SUITE(StopNoUnwind)

/* 栈上 RAII 哨兵：析构时置标志 */
struct StackGuard {
    std::atomic<int>* destroyed;
    explicit StackGuard(std::atomic<int>* d) : destroyed(d) {}
    ~StackGuard() { destroyed->fetch_add(1); }
};

/* BOOST_REQUIRE 失败（内部抛异常）时也必须停机：协程闭包按引用捕获测试栈上
 * 局部量，若 worker/调度线程在局部量析构后仍存活，继续触碰即为 UB。本驻留
 * 对象必须声明在全部被捕获局部量之后，保证析构顺序：先 Stop（join 全部线程），
 * 后销毁局部量。 */
struct SchedulerStopGuard
{
    ~SchedulerStopGuard()
    {
        if (g_scheduler->IsRunning())
            g_scheduler->Stop();
    }
};

/* 有界等待：轮询 flag 至多 ms 毫秒，返回是否置位 */
static bool WaitFlag(const std::atomic<bool>& flag, int ms)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!flag.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return flag.load();
}

static void InitSingleWorkerConfig()
{
    g_bbt_coroutine_config->m_cfg_static_thread_num = 1;
    g_bbt_coroutine_config->m_cfg_stack_size = 65536;
    g_bbt_coroutine_config->m_cfg_stack_protect = true;
}

BOOST_AUTO_TEST_CASE(t_stop_parked_coroutine_no_stack_unwind)
{
    InitSingleWorkerConfig();

    std::atomic<int> guard_destroyed{0};
    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};

    g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    BOOST_REQUIRE(g_scheduler->IsRunning());
    SchedulerStopGuard stop_guard;

    bbtco [&]() {
        started.store(true);
        StackGuard guard(&guard_destroyed);
        /* 挂起在超时事件上，永远不会自然超时 */
        g_bbt_tls_coroutine_co->YieldUntilTimeout(60000);
        /* 如果 Stop 后协程被恢复执行到这里，说明做了栈展开——不应该发生 */
        completed.store(true);
    };

    /* 等协程真正启动并挂起 */
    BOOST_REQUIRE(WaitFlag(started, 2000));
    /* 再等一拍确保进入 PARKED */
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    g_scheduler->Stop();

    /* Stop 后挂起协程被直接销毁：协程未恢复执行，StackGuard 析构不应执行 */
    BOOST_CHECK(!completed.load());
    BOOST_CHECK_EQUAL(guard_destroyed.load(), 0);
}

/**
 * #339 A4 回归：Stop 应释放经 CoWaiter::WaitWithCallback（custom 事件）挂起
 * 的协程任务闭包。契约依据：§6 Stop 直接销毁挂起协程（#338 用户决策），
 * Coroutine/Context 任务闭包属于运行时资源，应随销毁释放。
 *
 * 回归背景（基线 a8a655e，父探针三连验证）：custom 事件路径的挂起协程
 * 曾未纳入任何回收点，Stop 后闭包泄漏、Notify 仍可命中。该测试固定记录
 * 这一生命周期契约，修复后必须通过。
 *
 * 单 worker 时序约束：heartbeat 任务只有在首个协程真正挂起（让出 worker）
 * 后才能执行，以此证明挂起已提交，无需读取内部状态。
 */
BOOST_AUTO_TEST_CASE(t_stop_releases_custom_wait_task_closure)
{
    InitSingleWorkerConfig();

    g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    BOOST_REQUIRE(g_scheduler->IsRunning());

    auto waiter = sync::CoWaiter::Create();
    auto token = std::make_shared<int>(1);
    std::weak_ptr<int> witness = token;
    std::atomic<bool> entered{false};
    std::atomic<bool> heartbeat{false};
    std::atomic<bool> resumed{false};
    SchedulerStopGuard stop_guard;

    /* 闭包捕获 token 持有堆对象；token 在注册后立即由本线程释放，
     * 协程闭包成为唯一持有者——witness 失效与否完全取决于运行时是否释放闭包 */
    g_scheduler->RegistCoroutineTask([&, token] {
        entered.store(true);
        waiter->WaitWithCallback([] { return true; });
        resumed.store(true);
    });
    token.reset();

    /* 等协程启动并挂起（bounded，非无界等待） */
    BOOST_REQUIRE(WaitFlag(entered, 2000));

    /* 单 worker：后续任务能执行 ⇔ 前一协程已真正 park 并让出 worker */
    g_scheduler->RegistCoroutineTask([&] { heartbeat.store(true); });
    BOOST_REQUIRE(WaitFlag(heartbeat, 2000));

    g_scheduler->Stop();

    /* 契约：Stop 销毁挂起协程并释放任务闭包 → witness 必须失效 */
    BOOST_CHECK_MESSAGE(witness.expired(),
        "Stop 后 custom wait 挂起协程的任务闭包未被释放（堆捕获泄漏）");

    /* Stop 后 Notify 应失败：事件已被 UnRegist 置 CANCELLED，Trigger 不再回调 */
    BOOST_CHECK_MESSAGE(waiter->Notify() != 0,
        "Stop 后 CoWaiter::Notify 应失败（等待者已销毁），实际返回 0");

    BOOST_CHECK(!resumed.load());
}

/**
 * #339 A4 回归：协程 RequestCancel 后调用 WaitWithCallback 不应无限挂起。
 * 对照：Yield 族入口均有 IsCancelRequested() 快速返回（Coroutine.cc:168 等），
 * WaitWithCallback 经 _RegistAwaitEvent 公共登记路径获得同样的取消预检——
 * 挂起回调内先看取消位，已置位则 Trigger(TIMEOUT)，CommitPark 走 PENDING
 * 立即完成，协程自行返回，无需外部 Notify 兜底。
 *
 * 兜底设计（bounded）：控制线程只在协程未自行返回时唤醒一次，用兜底计数
 * 断言协程无需兜底即可返回。契约仅要求"不无限挂起"，不定义预取消返回码，
 * 故只断言返回，不校验返回值。
 */
BOOST_AUTO_TEST_CASE(t_wait_with_callback_after_request_cancel_returns)
{
    InitSingleWorkerConfig();

    g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
    BOOST_REQUIRE(g_scheduler->IsRunning());

    auto waiter = sync::CoWaiter::Create();
    std::atomic<bool> entered{false};
    std::atomic<bool> returned{false};
    /* 兜底唤醒计数：0 = 协程自己返回；1 = 需要兜底，表示取消预检回归 */
    std::atomic<int> fallback_wakes{0};
    SchedulerStopGuard stop_guard;

    g_scheduler->RegistCoroutineTask([&] {
        g_bbt_tls_coroutine_co->RequestCancel();
        entered.store(true);
        waiter->WaitWithCallback([] { return true; });
        returned.store(true);
    });

    BOOST_REQUIRE(WaitFlag(entered, 2000));

    /* bounded 兜底：控制线程只在协程未自行返回时唤醒一次 */
    if (!WaitFlag(returned, 2000)) {
        fallback_wakes.fetch_add(1);
        waiter->Notify();
        BOOST_REQUIRE(WaitFlag(returned, 2000));
    }

    BOOST_CHECK_MESSAGE(returned.load(),
        "WaitWithCallback 在 RequestCancel 后既未自行返回也未被兜底唤醒");
    BOOST_CHECK_EQUAL(fallback_wakes.load(), 0);

    g_scheduler->Stop();
}

/**
 * #339 A4：四种 Wait 族（Wait / WaitWithCallback / WaitWithTimeout /
 * WaitWithTimeoutAndCallback）的正常 Notify 唤醒与预取消自返回。
 *
 * 每个变体一轮独立 Start/Stop；预取消轮次不发送任何 Notify，协程必须
 * 自行返回（经 _RegistAwaitEvent 取消预检）——这是取消预检的回归保护。
 * 返回值断言：正常轮次为 0；预取消轮次只要求有界返回，不固化其内部唤醒事件
 * 对应返回码（取消结果码仍可按契约另行定义）。
 */
BOOST_AUTO_TEST_CASE(t_wait_family_normal_and_pre_cancel)
{
    enum class WaitKind { Plain, PlainCb, Timeout, TimeoutCb };

    for (auto kind : { WaitKind::Plain, WaitKind::PlainCb,
                       WaitKind::Timeout, WaitKind::TimeoutCb })
    {
        for (bool precancel : { false, true })
        {
            InitSingleWorkerConfig();
            g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
            BOOST_REQUIRE(g_scheduler->IsRunning());

            auto waiter = sync::CoWaiter::Create();
            std::atomic<bool> entered{false};
            std::atomic<bool> heartbeat{false};
            std::atomic<bool> returned{false};
            std::atomic<int> wait_ret{-99};
            /* 声明在全部被捕获局部量之后：REQUIRE 失败时先停调度再析构局部量 */
            SchedulerStopGuard stop_guard;

            g_scheduler->RegistCoroutineTask([&] {
                entered.store(true);
                if (precancel)
                    g_bbt_tls_coroutine_co->RequestCancel();
                int r = -99;
                switch (kind) {
                case WaitKind::Plain:
                    r = waiter->Wait();
                    break;
                case WaitKind::PlainCb:
                    r = waiter->WaitWithCallback([] { return true; });
                    break;
                case WaitKind::Timeout:
                    r = waiter->WaitWithTimeout(60000);
                    break;
                case WaitKind::TimeoutCb:
                    r = waiter->WaitWithTimeoutAndCallback(60000, [] { return true; });
                    break;
                }
                wait_ret.store(r);
                returned.store(true);
            });

            BOOST_REQUIRE(WaitFlag(entered, 2000));

            if (!precancel) {
                g_scheduler->RegistCoroutineTask([&] { heartbeat.store(true); });
                BOOST_REQUIRE(WaitFlag(heartbeat, 2000));
                BOOST_CHECK_EQUAL(waiter->Notify(), 0);
            }

            /* 预取消轮次无任何 Notify：协程必须自行返回，否则 bounded 超时 */
            BOOST_REQUIRE_MESSAGE(WaitFlag(returned, 3000),
                "Wait 族未自行返回（预取消）或未在 Notify 后恢复（正常）");

            if (precancel)
                BOOST_CHECK_NE(wait_ret.load(), -99);
            else
                BOOST_CHECK_EQUAL(wait_ret.load(), 0);

            g_scheduler->Stop();
        }
    }
}

/**
 * #339 A4：提前/重复 Notify 语义。
 * - 提前：等待尚未注册（无人挂起）时 Notify 必须失败（-1）；随后协程正常
 *   等待，丢失的提前通知由超时兜底，协程按时返回 1——不吞掉等待。
 * - 重复：唤醒成功的第二次 Notify 必须失败（事件已 FINAL 或 m_co_event 已
 *   清空，两个时序下都返回 -1，无需竞态防护）。
 */
BOOST_AUTO_TEST_CASE(t_wait_notify_early_and_repeat)
{
    InitSingleWorkerConfig();

    /* --- 提前 Notify：等待者尚未注册 --- */
    {
        auto early_waiter = sync::CoWaiter::Create();
        BOOST_CHECK_EQUAL(early_waiter->Notify(), -1);   // 无等待者，必须失败

        g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
        BOOST_REQUIRE(g_scheduler->IsRunning());
        std::atomic<bool> entered{false};
        std::atomic<bool> heartbeat{false};
        std::atomic<bool> returned{false};
        std::atomic<int> ret{-99};
        SchedulerStopGuard stop_guard;

        g_scheduler->RegistCoroutineTask([&] {
            entered.store(true);
            ret.store(early_waiter->WaitWithTimeout(300));  // 提前通知已丢失，靠超时返回
            returned.store(true);
        });
        BOOST_REQUIRE(WaitFlag(entered, 2000));
        g_scheduler->RegistCoroutineTask([&] { heartbeat.store(true); });
        BOOST_REQUIRE(WaitFlag(heartbeat, 2000));
        BOOST_REQUIRE(WaitFlag(returned, 3000));
        BOOST_CHECK_EQUAL(ret.load(), 1);   // 超时兜底，等待未被吞掉

        g_scheduler->Stop();
    }

    /* --- 重复 Notify：第二次必须失败 --- */
    {
        g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
        BOOST_REQUIRE(g_scheduler->IsRunning());
        auto waiter = sync::CoWaiter::Create();
        std::atomic<bool> entered{false};
        std::atomic<bool> heartbeat{false};
        std::atomic<bool> returned{false};
        std::atomic<int> ret{-99};
        SchedulerStopGuard stop_guard;

        g_scheduler->RegistCoroutineTask([&] {
            entered.store(true);
            ret.store(waiter->WaitWithTimeout(60000));
            returned.store(true);
        });
        BOOST_REQUIRE(WaitFlag(entered, 2000));
        g_scheduler->RegistCoroutineTask([&] { heartbeat.store(true); });
        BOOST_REQUIRE(WaitFlag(heartbeat, 2000));

        BOOST_CHECK_EQUAL(waiter->Notify(), 0);          // 第一次：唤醒成功
        BOOST_REQUIRE(WaitFlag(returned, 3000));
        BOOST_CHECK_EQUAL(ret.load(), 0);
        BOOST_CHECK_EQUAL(waiter->Notify(), -1);         // 第二次：必须失败

        g_scheduler->Stop();
    }
}

/**
 * #339 A4：Stop 后闭包释放 + 不展开，覆盖全部四种 Wait 族的 custom 等待。
 * 每次变体用独立 token 作闭包堆捕获 witness：Stop 必须销毁挂起协程（含任务
 * 闭包与栈），witness 失效；栈上 StackGuard 不执行析构（不展开）；Stop 后
 * Notify 失败。框架与 t_stop_releases_custom_wait_task_closure 相同，
 * 仅换等待原语，补齐 Wait / WaitWithTimeout / WaitWithTimeoutAndCallback。
 */
BOOST_AUTO_TEST_CASE(t_stop_releases_closures_all_wait_variants)
{
    enum class WaitKind { Plain, Timeout, TimeoutCb };

    for (auto kind : { WaitKind::Plain, WaitKind::Timeout, WaitKind::TimeoutCb })
    {
        InitSingleWorkerConfig();
        g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
        BOOST_REQUIRE(g_scheduler->IsRunning());

        auto waiter = sync::CoWaiter::Create();
        auto token = std::make_shared<int>(1);
        std::weak_ptr<int> witness = token;
        std::atomic<bool> entered{false};
        std::atomic<bool> heartbeat{false};
        std::atomic<bool> resumed{false};
        std::atomic<int> guard_destroyed{0};
        SchedulerStopGuard stop_guard;

        g_scheduler->RegistCoroutineTask([&, token] {
            entered.store(true);
            StackGuard sg(&guard_destroyed);
            switch (kind) {
            case WaitKind::Plain:
                waiter->Wait();
                break;
            case WaitKind::Timeout:
                waiter->WaitWithTimeout(60000);
                break;
            case WaitKind::TimeoutCb:
                waiter->WaitWithTimeoutAndCallback(60000, [] { return true; });
                break;
            }
            resumed.store(true);
        });
        token.reset();

        BOOST_REQUIRE(WaitFlag(entered, 2000));
        g_scheduler->RegistCoroutineTask([&] { heartbeat.store(true); });
        BOOST_REQUIRE(WaitFlag(heartbeat, 2000));

        g_scheduler->Stop();

        BOOST_CHECK_MESSAGE(witness.expired(), "Stop 后任务闭包未被释放（泄漏）");
        BOOST_CHECK_MESSAGE(waiter->Notify() != 0, "Stop 后 Notify 应失败");
        BOOST_CHECK(!resumed.load());
        BOOST_CHECK_EQUAL(guard_destroyed.load(), 0);   // 直接销毁，不做栈展开
    }
}

/**
 * #339 A4：Stop 与"已经开始的通知"竞争的有界回归。
 *
 * 竞态事实：用户线程 Notify 的 Trigger 可能已在 Stop 的 UnRegist 之前完成
 * PARKED→TRIGGERING CAS（唤醒在途）。责任链要求：DestroyParkedCoroutines
 * 对 UnRegist 失败的协程跳过删除（不踩在途回调 = 不 UAF），OnCoPollEvent 经
 * OnActiveCoroutine 在停机或代际失配时直接回收。队列排空与代际检查共同保证
 * 唤醒不会在下一次 Start 中执行旧协程。
 */
BOOST_AUTO_TEST_CASE(t_stop_races_started_notify)
{
    InitSingleWorkerConfig();

    for (int round = 0; round < 5; ++round)
    {
        g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
        BOOST_REQUIRE(g_scheduler->IsRunning());

        auto waiter = sync::CoWaiter::Create();
        std::atomic<bool> entered{false};
        std::atomic<bool> heartbeat{false};
        std::atomic<bool> resumed{false};
        SchedulerStopGuard stop_guard;

        g_scheduler->RegistCoroutineTask([&] {
            entered.store(true);
            waiter->WaitWithTimeout(60000);
            resumed.store(true);
        });

        /* 先证明挂起已提交，再让 Notify 与 Stop 正面竞争 */
        BOOST_REQUIRE(WaitFlag(entered, 2000));
        g_scheduler->RegistCoroutineTask([&] { heartbeat.store(true); });
        BOOST_REQUIRE(WaitFlag(heartbeat, 2000));

        std::thread notifier([&] { waiter->Notify(); });
        g_scheduler->Stop();
        notifier.join();

        /* 到达此处 = 竞争路径无崩溃/无死锁（Stop 内部有界） */
        BOOST_CHECK(!g_scheduler->IsRunning());
    }
}

/**
 * #339 A4：迟到的唤醒不得跨越 Stop/Start 代次。
 * 通知线程在第一次 Stop 返回时仍可能处于完成路径；立即 Start 新一代后，
 * 旧协程只能被回收，不能执行其任务闭包。测试只使用有界等待，不依赖调度
 * 线程恰好在某个时间点停顿。
 */
BOOST_AUTO_TEST_CASE(t_late_notify_cannot_run_old_generation)
{
    InitSingleWorkerConfig();

    for (int round = 0; round < 20; ++round)
    {
        g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
        BOOST_REQUIRE(g_scheduler->IsRunning());

        auto waiter = sync::CoWaiter::Create();
        auto token = std::make_shared<int>(1);
        std::weak_ptr<int> witness = token;
        std::atomic<bool> entered{false};
        std::atomic<bool> heartbeat{false};
        std::atomic<bool> restart_started{false};
        std::atomic<bool> ran_after_restart{false};
        SchedulerStopGuard stop_guard;

        g_scheduler->RegistCoroutineTask([&, token] {
            entered.store(true);
            waiter->WaitWithTimeout(60000);
            if (restart_started.load(std::memory_order_acquire))
                ran_after_restart.store(true, std::memory_order_release);
        });
        token.reset();

        BOOST_REQUIRE(WaitFlag(entered, 2000));
        g_scheduler->RegistCoroutineTask([&] { heartbeat.store(true); });
        BOOST_REQUIRE(WaitFlag(heartbeat, 2000));

        std::atomic<bool> notifier_started{false};
        std::thread notifier([&] {
            notifier_started.store(true, std::memory_order_release);
            waiter->Notify();
        });
        BOOST_CHECK(WaitFlag(notifier_started, 1000));

        g_scheduler->Stop();
        restart_started.store(true, std::memory_order_release);
        g_scheduler->Start(SCHE_START_OPT_SCHE_THREAD);
        BOOST_CHECK(g_scheduler->IsRunning());

        /* 先 join 完成路径，再停新一代，避免局部捕获在回调结束前析构。 */
        notifier.join();
        g_scheduler->Stop();

        BOOST_CHECK_MESSAGE(!ran_after_restart.load(),
            "上一代迟到唤醒在新一代 Start 后执行了旧协程");
        BOOST_CHECK_MESSAGE(witness.expired(),
            "上一代迟到唤醒导致旧协程任务闭包未释放");
    }
}

BOOST_AUTO_TEST_SUITE_END()