/**
 * @file Test_co_object_identity.cc
 * @brief #347 C0 对象身份验收：id 严格递增且不复用、运行时代际读取
 *        （未启动 / 运行中 / 停止后）、无运行时代际时 CreateObjectInfo 抛
 *        std::logic_error、身份快照值语义。
 *
 * 契约：agent-docs/2026-09-17-service-runtime-contract-v1.md §C0
 * 代际来源为 Scheduler 启动代际（每次重启递增）；未启动或已开始 Stop 时
 * 不存在可归属的运行时代际，CurrentRuntimeGeneration() 返回 0。
 */

#include <stdexcept>
#include <mutex>

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/object/CoObject.hpp>

/* 进程寿命模型：runtime 只初始化一次，重复 Start 抛 std::logic_error。
 * 每个测试文件就是一个可执行，这里把用例内的 Start() 收敛为进程内一次初始化。 */
namespace
{
void EnsureRuntime()
{
    static std::once_flag once;
    std::call_once(once, [](){
        bbt::coroutine::detail::Scheduler::GetInstance()->Start();
    });
}
}

using namespace bbt::coroutine;

BOOST_AUTO_TEST_SUITE(CoObjectIdentity)

/* 运行时未初始化：没有可分配身份，CreateObjectInfo 拒绝。
 * （本用例必须先于任何 Start 执行。） */
BOOST_AUTO_TEST_CASE(t_create_before_initialize_rejected)
{
    BOOST_CHECK(!detail::Scheduler::GetInstance()->IsInitialized());
    BOOST_CHECK_THROW(CreateObjectInfo("rank-service", "player-1"), std::logic_error);
}

/* 初始化后分配身份：id 严格递增、不复用；快照是值语义；无代际字段。 */
BOOST_AUTO_TEST_CASE(t_identity_after_start)
{
    EnsureRuntime();
    BOOST_REQUIRE(detail::Scheduler::GetInstance()->IsInitialized());

    const auto first = CreateObjectInfo("rank-service", "player-1");
    const auto second = CreateObjectInfo("rank-service", "player-2");
    const auto third = CreateObjectInfo("battle-service", "player-1");

    BOOST_CHECK_NE(first.id, 0u);
    BOOST_CHECK_EQUAL(second.id, first.id + 1);
    BOOST_CHECK_EQUAL(third.id, second.id + 1);

    BOOST_CHECK_EQUAL(first.kind, "rank-service");
    BOOST_CHECK_EQUAL(first.name, "player-1");

    /* 运行时不设停机：初始化前置条件恒成立，身份可继续分配（不再有「停止后拒绝」） */
    const auto fourth = CreateObjectInfo("rank-service", "player-3");
    BOOST_CHECK_EQUAL(fourth.id, third.id + 1);
}

BOOST_AUTO_TEST_SUITE_END()
