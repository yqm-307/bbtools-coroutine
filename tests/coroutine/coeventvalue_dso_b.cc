#include "coeventvalue_dso_types.hpp"

/* 属性 B：在另一个产物内读取 A 构造的载荷。 */
extern "C" bool dso_b_read(
    const bbt::coroutine::CoEventValue& value,
    std::int64_t* seq, std::uint32_t* code)
{
    CoroutineWakePayload payload{};
    if (!value.Get(payload))
        return false;
    if (seq != nullptr) *seq = payload.seq;
    if (code != nullptr) *code = payload.code;
    return true;
}

/* 同尺寸但不同登记 ID：必须拒绝且不改写 out。 */
extern "C" bool dso_b_read_wrong_type(const bbt::coroutine::CoEventValue& value)
{
    OtherWakePayload other{777, 777};
    if (value.Get(other))
        return false;
    return other.seq == 777 && other.code == 777u;
}

extern "C" std::uint32_t dso_b_payload_type_id()
{
    return bbt::coroutine::CoEventValueTraits<CoroutineWakePayload>::id;
}

/* 同尺寸不同身份（const void* 的不透明句柄 vs void* const* 的指针槽）：
 * 必须拒绝，且不得改写调用方 out。 */
extern "C" bool dso_b_read_as_pointer_slot(const bbt::coroutine::CoEventValue& value)
{
    void* const* slot = nullptr;
    if (value.Get(slot))
        return false;
    return slot == nullptr;
}

extern "C" bool dso_b_read_opaque(const bbt::coroutine::CoEventValue& value)
{
    const void* p = nullptr;
    return value.Get(p);
}

extern "C" std::uint32_t dso_b_opaque_type_id()
{
    return bbt::coroutine::CoEventValueTraits<const void*>::id;
}
