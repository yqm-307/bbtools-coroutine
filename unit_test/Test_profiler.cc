#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Profiler.hpp>
#include <bbt/coroutine/detail/Processer.hpp>

BOOST_AUTO_TEST_SUITE(ProfilerTest)

// 事件计数：各 OnEvent 接口的计数语义
BOOST_AUTO_TEST_CASE(t_event_counters)
{
    auto& profiler = bbt::coroutine::detail::Profiler::GetInstance();
    BOOST_REQUIRE(profiler != nullptr);
    profiler->Reset();

    constexpr int kN = 50;
    for (int i = 0; i < kN; ++i) {
        profiler->OnEvent_RegistCoroutine();
        profiler->OnEvent_DoneCoroutine();
        profiler->OnEvent_RegistCoPollEvent();
        profiler->OnEvent_TriggerCoPollEvent();
        profiler->OnEvent_StealSucc(1);
        profiler->OnEvent_StackAlloc();
        profiler->OnEvent_StackRelease(1);
        profiler->OnEvent_CoMutexLockYield();
    }
    for (int i = 0; i < kN; ++i)
        profiler->OnEvent_CreateCoroutine();
    for (int i = 0; i < kN / 2; ++i)
        profiler->OnEvent_DestoryCoroutine();

    // ProfileInfo 字符串应包含各统计字段且不崩溃
    std::string info;
    profiler->ProfileInfo(info);
    BOOST_TEST(!info.empty());
    BOOST_TEST(info.find("完成协程数") != std::string::npos);
    BOOST_TEST(info.find("注册协程数") != std::string::npos);
    BOOST_TEST(info.find("Steal数量") != std::string::npos);
    BOOST_TEST(info.find("CoEvent 注册数量") != std::string::npos);
    BOOST_TEST(info.find("CoEvent 触发数量") != std::string::npos);
    BOOST_TEST(info.find("CoMutex竞争导致挂起次数") != std::string::npos);
    BOOST_TEST(info.find("StackPool 指标") != std::string::npos);
    BOOST_TEST(info.find("未释放协程数") != std::string::npos);
    // 未释放 = create - destory = 50 - 25 = 25
    BOOST_TEST(info.find(std::to_string(kN - kN / 2)) != std::string::npos);
}

// 边界：StartScheudler 后立即 ProfileInfo（elapsed=0 除零回归）
BOOST_AUTO_TEST_CASE(t_profileinfo_zero_elapsed)
{
    auto& profiler = bbt::coroutine::detail::Profiler::GetInstance();
    BOOST_REQUIRE(profiler != nullptr);

    // 连续调用两次 StartScheudler 使 begin 与 now 同刻
    profiler->OnEvent_StartScheudler();

    std::string info;
    profiler->ProfileInfo(info);  // 修复前此处 elapsed=0 → 除零 UB
    BOOST_TEST(!info.empty());
    BOOST_TEST(info.find("已运行时间(ms)：") != std::string::npos);
}

// 边界：0 协程时的 DumpStderr（不崩溃、无 processer 时循环体跳过）
BOOST_AUTO_TEST_CASE(t_dumpstderr_empty)
{
    auto& profiler = bbt::coroutine::detail::Profiler::GetInstance();
    BOOST_REQUIRE(profiler != nullptr);

    profiler->OnEvent_StartScheudler();
    profiler->DumpStderr();  // 无 processer 注册时循环体为空
    BOOST_TEST(true);
}

// Processer 生命周期事件：Start/Stop 统计
BOOST_AUTO_TEST_CASE(t_processer_lifecycle)
{
    auto& profiler = bbt::coroutine::detail::Profiler::GetInstance();
    BOOST_REQUIRE(profiler != nullptr);
    profiler->Reset();

    auto proc = bbt::coroutine::detail::Processer::Create();
    BOOST_REQUIRE(proc != nullptr);

    profiler->OnEvent_StartProcesser(proc);

    std::string info;
    profiler->ProfileInfo(info);
    // processer 段应包含该 processer 的 PID
    BOOST_TEST(info.find("PID：") != std::string::npos);

    profiler->OnEvent_StopPorcesser(proc);

    info.clear();
    profiler->ProfileInfo(info);
    // 停止后 processer 段不再有 PID 条目
    BOOST_TEST(info.find("PID：") == std::string::npos);
}

// 集成：PROFILE 构建下 scheduler 运行协程后 ProfileInfo 可反映活动
// （非 PROFILE 构建时事件不回传，仅验证不崩溃）
BOOST_AUTO_TEST_CASE(t_scheduler_run_then_profile)
{
    g_scheduler->Start();

    bbt::core::thread::CountDownLatch done{10};
    for (int i = 0; i < 10; ++i) {
        bbtco [&done]() {
            bbtco_sleep(1);
            done.Down();
        };
    }

    auto deadline = bbt::core::clock::nowAfter(bbt::core::clock::milliseconds(5000));
    while (!bbt::core::clock::is_expired<bbt::core::clock::milliseconds>(deadline)) {
        if (done.WaitTimeout(1) == 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    auto& profiler = bbt::coroutine::detail::Profiler::GetInstance();
    std::string info;
    profiler->ProfileInfo(info);
    BOOST_TEST(!info.empty());

    g_scheduler->Stop();
}

BOOST_AUTO_TEST_SUITE_END()
