#pragma once
#include <cstdint>
#include <bbt/coroutine/sync/CoEventValue.hpp>

/**
 * @file coeventvalue_dso_types.hpp
 * @brief 跨 DSO 共享的自定义 POD 与其唯一登记 ID。
 *
 * 两个独立产物（coeventvalue_dso_a / _b）各自包含本头，使用同一常量 ID；
 * 因此一侧 From、另一侧 Get 必须识别为同类型，而另一个已登记类型必须被拒绝。
 * 这验证的是「显式登记 ID」而非模板静态地址或 RTTI。
 */

struct CoroutineWakePayload
{
    std::int64_t seq{0};
    std::uint32_t code{0};
};
BBT_COEVENT_VALUE_REGISTER_TYPE(CoroutineWakePayload, 0x101);

/* 与 CoroutineWakePayload 同尺寸、不同登记 ID：必须互相拒绝 */
struct OtherWakePayload
{
    std::int64_t seq{0};
    std::uint32_t code{0};
};
BBT_COEVENT_VALUE_REGISTER_TYPE(OtherWakePayload, 0x102);
