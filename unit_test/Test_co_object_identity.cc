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

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <bbt/coroutine/coroutine.hpp>
#include <bbt/coroutine/detail/Scheduler.hpp>
#include <bbt/coroutine/object/CoObject.hpp>

using namespace bbt::coroutine;

BOOST_AUTO_TEST_SUITE(CoObjectIdentity)

/* C-01：未启动运行时，没有可归属的运行时代际 */
BOOST_AUTO_TEST_CASE(t_no_runtime_generation)
{
    BOOST_CHECK_EQUAL(CurrentRuntimeGeneration(), 0u);
    BOOST_CHECK_THROW(CreateObjectInfo("rank-service", "player-1"), std::logic_error);
}

/* C-01：运行中分配身份；id 严格递增、不复用；代际与运行时一致 */
BOOST_AUTO_TEST_CASE(t_identity_after_start)
{
    auto& scheduler = detail::Scheduler::GetInstance();
    scheduler->Start();
    BOOST_REQUIRE_NE(CurrentRuntimeGeneration(), 0u);

    const auto generation = CurrentRuntimeGeneration();

    const auto first = CreateObjectInfo("rank-service", "player-1");
    const auto second = CreateObjectInfo("rank-service", "player-2");
    const auto third = CreateObjectInfo("battle-service", "player-1");

    /* 不同 kind/name 不共享 id 空间，id 全局唯一且严格递增 */
    BOOST_CHECK_EQUAL(first.generation, generation);
    BOOST_CHECK_EQUAL(second.generation, generation);
    BOOST_CHECK_NE(first.id, 0u);
    BOOST_CHECK_EQUAL(second.id, first.id + 1);
    BOOST_CHECK_EQUAL(third.id, second.id + 1);

    /* 快照是值语义，身份不授予操作权限 */
    BOOST_CHECK_EQUAL(first.kind, "rank-service");
    BOOST_CHECK_EQUAL(first.name, "player-1");

    scheduler->Stop();

    /* C-01：Stop 开始后不再有运行时代际 */
    BOOST_CHECK_EQUAL(CurrentRuntimeGeneration(), 0u);
    BOOST_CHECK_THROW(CreateObjectInfo("rank-service", "player-3"), std::logic_error);
}

BOOST_AUTO_TEST_SUITE_END()
