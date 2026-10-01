#pragma once
#include <cstdint>
#include <string>

namespace bbt::coroutine
{

using CoObjectId = std::uint64_t;

/**
 * @brief 对象身份快照：进程内唯一且永不复用的 id + 可读描述。
 *
 * 不再携带运行时代际：运行时不设停机/重启，「对象属于哪一代」不再是可观察
 * 语义；创建前置条件改为「运行时已初始化」。
 */
struct CoObjectInfo
{
    CoObjectId          id{0};
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
