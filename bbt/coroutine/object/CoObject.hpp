#pragma once
#include <string>
#include <bbt/coroutine/object/interface/ICoObject.hpp>

namespace bbt::coroutine
{

/**
 * @brief 分配一个新的对象身份快照。
 *
 * 运行时尚未初始化时抛 std::logic_error；id 溢出时抛 std::overflow_error。
 * id 进程内严格递增、不复用；没有代际字段。
 */
CoObjectInfo CreateObjectInfo(std::string kind, std::string name);

} // namespace bbt::coroutine
