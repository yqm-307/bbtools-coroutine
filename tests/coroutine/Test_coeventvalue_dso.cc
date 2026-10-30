/**
 * @file Test_coeventvalue_dso.cc
 * @brief 跨 DSO 类型识别：coeventvalue_dso_a 构造载荷，coeventvalue_dso_b
 *        读取；同类型识别一致、另一同尺寸登记类型被拒绝。
 *
 * 这是「不依赖模板局部地址/RTTI 做类型身份」的实测证据：两个 .so 各有一份
 * traits 实例，识别依赖公共头里的显式登记 ID 常量。
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <cstdint>

#include "coeventvalue_dso_types.hpp"

extern "C" bbt::coroutine::CoEventValue dso_a_store(std::int64_t seq, std::uint32_t code);
extern "C" std::uint32_t dso_a_payload_type_id();
extern "C" bbt::coroutine::CoEventValue dso_a_store_opaque(const void* p);
extern "C" std::uint32_t dso_a_opaque_type_id();
extern "C" bool dso_b_read(
    const bbt::coroutine::CoEventValue& value, std::int64_t* seq, std::uint32_t* code);
extern "C" bool dso_b_read_wrong_type(const bbt::coroutine::CoEventValue& value);
extern "C" std::uint32_t dso_b_payload_type_id();
extern "C" bool dso_b_read_as_pointer_slot(const bbt::coroutine::CoEventValue& value);
extern "C" bool dso_b_read_opaque(const bbt::coroutine::CoEventValue& value);
extern "C" std::uint32_t dso_b_opaque_type_id();

BOOST_AUTO_TEST_SUITE(CoEventValueDsoTest)

BOOST_AUTO_TEST_CASE(t_type_id_identical_across_dso)
{
    BOOST_CHECK_EQUAL(dso_a_payload_type_id(), dso_b_payload_type_id());
    BOOST_CHECK_EQUAL(dso_a_payload_type_id(),
                      bbt::coroutine::CoEventValueTraits<CoroutineWakePayload>::id);
}

BOOST_AUTO_TEST_CASE(t_store_in_a_read_in_b)
{
    const auto value = dso_a_store(7, 42);

    std::int64_t seq = -1;
    std::uint32_t code = 0;
    BOOST_CHECK(dso_b_read(value, &seq, &code));
    BOOST_CHECK_EQUAL(seq, 7);
    BOOST_CHECK_EQUAL(code, 42u);

    /* 另一同尺寸登记类型必须被拒绝，且不留下半成品 */
    BOOST_CHECK(dso_b_read_wrong_type(value));

    /* 本侧（第三个编译单元）同样读回同一载荷 */
    CoroutineWakePayload payload{};
    BOOST_CHECK(value.Get(payload));
    BOOST_CHECK_EQUAL(payload.seq, 7);
    BOOST_CHECK_EQUAL(payload.code, 42u);
}

/* 跨 DSO 的 void 族身份：const void*（不透明句柄）与 void* const*（指针槽）
 * 同尺寸，两个产物必须给出互异 ID；B 侧用错型读必须失败且 out 不变。 */
BOOST_AUTO_TEST_CASE(t_void_family_identity_across_dso)
{
    BOOST_CHECK_EQUAL(dso_a_opaque_type_id(), dso_b_opaque_type_id());
    BOOST_CHECK(dso_a_opaque_type_id() !=
                bbt::coroutine::CoEventValueTraits<void* const*>::id);

    int dummy = 0;
    const auto opaque = dso_a_store_opaque(&dummy);

    BOOST_CHECK(dso_b_read_as_pointer_slot(opaque));   // 拒绝且 out 未被改写
    BOOST_CHECK(dso_b_read_opaque(opaque));            // 同类型可读回
}

BOOST_AUTO_TEST_SUITE_END()
