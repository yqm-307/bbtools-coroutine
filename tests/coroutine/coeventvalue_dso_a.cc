#include "coeventvalue_dso_types.hpp"

/* 属性 A：在本产物内构造载荷；同类型 ID 必须被另一产物识别。 */
extern "C" bbt::coroutine::CoEventValue dso_a_store(std::int64_t seq, std::uint32_t code)
{
    return bbt::coroutine::CoEventValue::From(CoroutineWakePayload{seq, code});
}

extern "C" std::uint32_t dso_a_payload_type_id()
{
    return bbt::coroutine::CoEventValueTraits<CoroutineWakePayload>::id;
}

/* 属性 A：跨 DSO 传递不透明句柄（const void*）。与 void* const*（指针槽）
 * 同尺寸、历史上同 ID，必须被另一产物按身份区分。 */
extern "C" bbt::coroutine::CoEventValue dso_a_store_opaque(const void* p)
{
    return bbt::coroutine::CoEventValue::From(p);
}

extern "C" std::uint32_t dso_a_opaque_type_id()
{
    return bbt::coroutine::CoEventValueTraits<const void*>::id;
}
