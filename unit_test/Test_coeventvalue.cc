/**
 * @file Test_coeventvalue.cc
 * @brief CoEventValue 内联载荷验证：24B 容量、显式 type-id、错型拒绝、
 *        空值语义、指针借用不延寿。
 *
 * 契约：本仓公共头 bbt/coroutine/sync/CoEventValue.hpp；容量与 type-id
 * 方案见 coroutine process-lifetime 公共迁移确认稿（显式稳定 ID + traits）。
 * 跨 DSO 同类型识别在 Test_coeventvalue_dso 覆盖（两个独立产物）。
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <cstdint>
#include <type_traits>

#include <bbt/coroutine/sync/CoEventValue.hpp>

using namespace bbt::coroutine;

namespace
{

/* 与本文件内其它类型同尺寸、但登记 ID 不同：必须互相拒绝 */
struct SameSizeOtherType
{
    std::int64_t seq;
    std::uint32_t code{0};
};

struct Pod23
{
    unsigned char bytes[23];
};

struct Pod24
{
    unsigned char bytes[24];
};

/* 自定义登记区间上界（0xFFFF）：指针层级容量的最坏基数 */
struct HugeIdPayload
{
    unsigned char bytes[8];
};

} // namespace

/* 登记写在全局命名空间作用域：显式特化只能声明在包裹模板命名空间的命名空间里
 * （与 std::hash 特化同一语言规则），放进匿名/用户命名空间会报 “does not enclose”。 */
BBT_COEVENT_VALUE_REGISTER_TYPE(SameSizeOtherType, 0x201);
BBT_COEVENT_VALUE_REGISTER_TYPE(Pod23, 0x202);
BBT_COEVENT_VALUE_REGISTER_TYPE(Pod24, 0x203);
BBT_COEVENT_VALUE_REGISTER_TYPE(HugeIdPayload, 0xFFFF);

BOOST_AUTO_TEST_SUITE(CoEventValueTest)

BOOST_AUTO_TEST_CASE(t_default_is_empty_and_get_rejects)
{
    CoEventValue empty;
    BOOST_CHECK(!empty.HasValue());

    std::int64_t out{123};
    BOOST_CHECK(!empty.Get(out));
    BOOST_CHECK_EQUAL(out, 123);   // 失败不得修改 out
}

BOOST_AUTO_TEST_CASE(t_integer_roundtrip)
{
    const auto v = CoEventValue::From(std::int64_t{7});
    BOOST_CHECK(v.HasValue());

    std::int64_t out{0};
    BOOST_CHECK(v.Get(out));
    BOOST_CHECK_EQUAL(out, 7);

    /* 基础类型的登记 ID 是公共常量，跨 TU 一致 */
    BOOST_CHECK_EQUAL(CoEventValueTraits<std::int64_t>::id, kCoEventValueInt64);
    BOOST_CHECK_EQUAL(CoEventValueTraits<int>::id, kCoEventValueInt);
}

BOOST_AUTO_TEST_CASE(t_same_size_other_type_rejected)
{
    const auto v = CoEventValue::From(std::int64_t{7});

    SameSizeOtherType other{99, 99};
    BOOST_CHECK(!v.Get(other));            // 仅按 sizeof 校验会误判为通过
    BOOST_CHECK_EQUAL(other.seq, 99);      // 失败不得修改 out
    BOOST_CHECK_EQUAL(other.code, 99u);

    /* 同类型仍可正常读回 */
    std::int64_t ok{0};
    BOOST_CHECK(v.Get(ok));
    BOOST_CHECK_EQUAL(ok, 7);
}

BOOST_AUTO_TEST_CASE(t_capacity_boundary_23_24)
{
    Pod23 p23{};
    for (std::size_t i = 0; i < sizeof(p23.bytes); ++i)
        p23.bytes[i] = static_cast<unsigned char>(i + 1);
    const auto v23 = CoEventValue::From(p23);
    Pod23 r23{};
    BOOST_CHECK(v23.Get(r23));
    BOOST_CHECK_EQUAL(r23.bytes[0], 1);
    BOOST_CHECK_EQUAL(r23.bytes[22], 23);

    Pod24 p24{};
    for (std::size_t i = 0; i < sizeof(p24.bytes); ++i)
        p24.bytes[i] = static_cast<unsigned char>(i + 5);
    const auto v24 = CoEventValue::From(p24);   // 恰好 24B：必须可编译可读回
    Pod24 r24{};
    BOOST_CHECK(v24.Get(r24));
    BOOST_CHECK_EQUAL(r24.bytes[23], 28);
}

BOOST_AUTO_TEST_CASE(t_pointer_borrow_per_pointee_id)
{
    std::int64_t target = 42;
    const auto v = CoEventValue::From(&target);

    std::int64_t* out = nullptr;
    BOOST_CHECK(v.Get(out));
    BOOST_CHECK(out == &target);          // 只借用地址，不延寿、不复制对象

    /* 不同被指类型的指针 ID 不同：不得塌缩成统一 Pointer */
    double* wrong = nullptr;
    BOOST_CHECK(!v.Get(wrong));
    BOOST_CHECK(wrong == nullptr);

    BOOST_CHECK(CoEventValueTraits<std::int64_t*>::id !=
                CoEventValueTraits<double*>::id);
    BOOST_CHECK(CoEventValueTraits<void*>::id != CoEventValueTraits<void>::id);
    BOOST_CHECK(CoEventValueTraits<void*>::id ==
                kCoEventValuePointerBegin + kCoEventValueVoid * 4u);
}

BOOST_AUTO_TEST_CASE(t_custom_id_out_of_range_rejected_at_compile_time)
{
    /* 自定义 ID 区间契约：>= kCoEventValueCustomBegin 且 < kCoEventValuePointerBegin。
     * 越界登记由 traits 里的 static_assert 拒绝（负向编译检查见 CMake 的
     * BBT_COEVENT_VALUE_TRAITS_RANGE_CHECK）。 */
    BOOST_CHECK(CoEventValueTraits<SameSizeOtherType>::id >= kCoEventValueCustomBegin);
    BOOST_CHECK(CoEventValueTraits<SameSizeOtherType>::id < kCoEventValuePointerBegin);
}

/* 基础类型身份：char / signed char / unsigned char 是三种不同类型，ID 必须互异，
 * 不能因 sizeof 相同或 char 的符号性而互相识别。 */
BOOST_AUTO_TEST_CASE(t_char_family_distinct)
{
    BOOST_CHECK(CoEventValueTraits<char>::id != CoEventValueTraits<signed char>::id);
    BOOST_CHECK(CoEventValueTraits<signed char>::id != CoEventValueTraits<unsigned char>::id);
    BOOST_CHECK(CoEventValueTraits<char>::id != CoEventValueTraits<unsigned char>::id);

    const char c = 'A';
    signed char sc = 0;
    BOOST_CHECK(!CoEventValue::From(c).Get(sc));   // char 不得读成 signed char
    BOOST_CHECK_EQUAL(sc, 0);                      // 失败不得修改 out
    char back = 0;
    BOOST_CHECK(CoEventValue::From(c).Get(back));
    BOOST_CHECK_EQUAL(back, 'A');

    signed char s2 = -5;
    signed char s3 = 0;
    BOOST_CHECK(CoEventValue::From(s2).Get(s3));
    BOOST_CHECK_EQUAL(s3, -5);
}

/* LP64 下 long long 与 int64_t（= long）是不同类型，不得共享 64 位登记位；
 * 若某平台 int64_t 恰好就是 long long，则退化为「同类型同 ID」。 */
BOOST_AUTO_TEST_CASE(t_long_long_vs_int64_alias)
{
    if constexpr (!std::is_same<long long, std::int64_t>::value) {
        BOOST_CHECK(CoEventValueTraits<long long>::id !=
                    CoEventValueTraits<std::int64_t>::id);
        const auto v = CoEventValue::From(7LL);
        std::int64_t got = 0;
        BOOST_CHECK(!v.Get(got));      // long long 不得读成 int64_t
        BOOST_CHECK_EQUAL(got, 0);
        long long ll = 0;
        BOOST_CHECK(v.Get(ll));
        BOOST_CHECK_EQUAL(ll, 7);
    } else {
        BOOST_CHECK_EQUAL(CoEventValueTraits<long long>::id,
                          CoEventValueTraits<std::int64_t>::id);
    }
}

/* 被指 cv 与指针层级都是身份的一部分：const int* 不得读成 int*，
 * 二级指针不得读成一级指针。 */
BOOST_AUTO_TEST_CASE(t_pointer_pointee_cv_and_depth)
{
    BOOST_CHECK(CoEventValueTraits<const int*>::id != CoEventValueTraits<int*>::id);
    BOOST_CHECK(CoEventValueTraits<volatile int*>::id != CoEventValueTraits<int*>::id);
    BOOST_CHECK(CoEventValueTraits<int**>::id != CoEventValueTraits<int*>::id);

    const int original = 7;
    const int* cp = &original;
    int* mutablep = nullptr;
    const auto v = CoEventValue::From(cp);
    BOOST_CHECK(!v.Get(mutablep));      // 不得丢弃被指 const
    BOOST_CHECK(mutablep == nullptr);
    const int* cp_out = nullptr;
    BOOST_CHECK(v.Get(cp_out));
    BOOST_CHECK(cp_out == &original);

    int* p = nullptr;
    int** pp = &p;
    int* p_out = nullptr;
    BOOST_CHECK(!CoEventValue::From(pp).Get(p_out));   // 二级不得读成一级
    BOOST_CHECK(p_out == nullptr);
    int** pp_out = nullptr;
    BOOST_CHECK(CoEventValue::From(pp).Get(pp_out));
    BOOST_CHECK(pp_out == &p);
}

/* void 与 void* 的身份分离（r1 已证 3 组碰撞）：
 *  - const void* 与 void* const*、volatile / const volatile 两对历史同为
 *    65585/65586/65587，错型 Get 能按 sizeof 读回 8 字节并改写 out；
 *  - 根因是 cv-void 指针的全特化与通用指针公式数值重合（void 与 void*
 *    共用登记 ID 12）。 */
BOOST_AUTO_TEST_CASE(t_void_pointer_family_ids_distinct)
{
    BOOST_CHECK(CoEventValueTraits<const void*>::id !=
                CoEventValueTraits<void* const*>::id);
    BOOST_CHECK(CoEventValueTraits<volatile void*>::id !=
                CoEventValueTraits<void* volatile*>::id);
    BOOST_CHECK(CoEventValueTraits<const volatile void*>::id !=
                CoEventValueTraits<void* const volatile*>::id);

    BOOST_CHECK(CoEventValueTraits<void*>::id != CoEventValueTraits<const void*>::id);
    BOOST_CHECK(CoEventValueTraits<void>::id == kCoEventValueVoid);

    /* 端到端：const void* 载荷不得被 void* const* 读回，且 out 不变 */
    int dummy = 0;
    const void* cp = &dummy;
    const auto v = CoEventValue::From(cp);

    void* const* poc = nullptr;
    BOOST_CHECK(!v.Get(poc));
    BOOST_CHECK(poc == nullptr);

    int* const* ic = nullptr;
    BOOST_CHECK(!v.Get(ic));
    BOOST_CHECK(ic == nullptr);

    const void* cp_out = nullptr;
    BOOST_CHECK(v.Get(cp_out));
    BOOST_CHECK(cp_out == &dummy);

    /* 反向：void* const* 载荷不得被 const void* 读回 */
    void* p = &dummy;
    void* const* ppv = &p;
    const auto v2 = CoEventValue::From(ppv);

    const void* back = nullptr;
    BOOST_CHECK(!v2.Get(back));
    BOOST_CHECK(back == nullptr);

    void* const* ppv_out = nullptr;
    BOOST_CHECK(v2.Get(ppv_out));
    BOOST_CHECK(ppv_out == &p);
}

/* 指针层级容量：最高合法层级（kCoEventValueMaxPointerDepth=7）必须可区分、
 * 可往返，且最坏基数（自定义登记区间上界）下逐层严格递增（无 uint32 回绕）。
 * 更深层级由 CMake 负向编译检查（too_deep.cc）拒绝，不做静默回绕。 */
BOOST_AUTO_TEST_CASE(t_pointer_depth_within_capacity)
{
    using I1 = int*;
    using I2 = I1*;
    using I3 = I2*;
    using I4 = I3*;
    using I5 = I4*;
    using I6 = I5*;
    using I7 = I6*;

    BOOST_CHECK_EQUAL(kCoEventValueMaxPointerDepth, 7u);

    const CoEventValueTypeId ids[7] = {
        CoEventValueTraits<I1>::id, CoEventValueTraits<I2>::id,
        CoEventValueTraits<I3>::id, CoEventValueTraits<I4>::id,
        CoEventValueTraits<I5>::id, CoEventValueTraits<I6>::id,
        CoEventValueTraits<I7>::id,
    };
    for (int i = 0; i < 7; ++i)
        for (int j = i + 1; j < 7; ++j)
            BOOST_CHECK_MESSAGE(ids[i] != ids[j], "pointer depth " << i + 1
                << " and " << j + 1 << " share id " << ids[i]);

    int level = 0;
    I7 p7 = reinterpret_cast<I7>(&level);
    const auto v = CoEventValue::From(p7);
    I7 out = nullptr;
    BOOST_CHECK(v.Get(out));
    BOOST_CHECK(out == p7);
    I6 wrong = nullptr;
    BOOST_CHECK(!v.Get(wrong));          // 7 层不得读成 6 层
    BOOST_CHECK(wrong == nullptr);

    /* 最坏基数：0xFFFF 登记 ID 的 7 层指针必须逐层严格递增（回绕会让
     * 高层的 ID 落回低位区间，进而与低层类型撞号） */
    using H1 = HugeIdPayload*;
    using H2 = H1*;
    using H3 = H2*;
    using H4 = H3*;
    using H5 = H4*;
    using H6 = H5*;
    using H7 = H6*;
    const CoEventValueTypeId worst[7] = {
        CoEventValueTraits<H1>::id, CoEventValueTraits<H2>::id,
        CoEventValueTraits<H3>::id, CoEventValueTraits<H4>::id,
        CoEventValueTraits<H5>::id, CoEventValueTraits<H6>::id,
        CoEventValueTraits<H7>::id,
    };
    for (int i = 1; i < 7; ++i)
        BOOST_CHECK_MESSAGE(worst[i] > worst[i - 1],
            "worst-case pointer depth " << i + 1 << " id " << worst[i]
            << " did not grow past " << worst[i - 1] << " (uint32 overflow)");
    /* 每层乘 4 → 每层比内层大至少 3 倍，末层远小于 uint32 上界 */
    BOOST_CHECK(worst[6] < 0xFFFFFFFFu / 3u);
}

BOOST_AUTO_TEST_SUITE_END()
