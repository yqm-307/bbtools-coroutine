#pragma once
#include <string>
#include <bbt/coroutine/object/interface/ICoObject.hpp>

namespace bbt::coroutine
{

/**
 * @brief 分配一个新的对象身份快照。
 *
 * 运行时代际为 0（未启动或已停止）时抛 std::logic_error；
 * id 溢出时抛 std::overflow_error。
 */
CoObjectInfo CreateObjectInfo(std::string kind, std::string name);

/**
 * @brief 当前运行时代际；0 表示运行时未启动或已停止。
 */
RuntimeGeneration CurrentRuntimeGeneration() noexcept;

} // namespace bbt::coroutine
