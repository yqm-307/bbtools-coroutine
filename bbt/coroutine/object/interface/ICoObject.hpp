#pragma once
#include <cstdint>
#include <string>

namespace bbt::coroutine
{

using CoObjectId = std::uint64_t;
using RuntimeGeneration = std::uint64_t;

/**
 * @brief 对象身份快照：id 与创建时的运行时代际。
 *
 * id 在单个进程内严格递增、永不复用；generation 为 0 表示对象不是在
 * 一个已启动的运行时里创建的。
 */
struct CoObjectInfo
{
    CoObjectId          id{0};
    RuntimeGeneration   generation{0};
    std::string         kind;
    std::string         name;
};

/**
 * @brief 受管对象的最小身份接口。
 *
 * 只约束「能提供自己的身份」，不约束生命周期、执行域或网络能力；
 * 后者由更具体的接口分别描述。
 */
class ICoObject
{
public:
    virtual ~ICoObject() = default;

    virtual CoObjectInfo GetObjectInfo() const = 0;
};

} // namespace bbt::coroutine
