/**
 * @file ICoObject.hpp
 * @brief 受管对象身份快照的最小接口。
 */
#pragma once
#include <cstdint>
#include <string>

namespace bbt::coroutine {

using CoObjectId = std::uint64_t;

/**
 * @brief 对象身份快照：进程内唯一且永不复用的 id + 可读描述。
 *
 * 只描述身份，不带运行时代际——运行时没有停机/重启，创建前置条件是
 * 「运行时已初始化」。
 */
struct CoObjectInfo {
    /**
     * 进程内唯一、严格递增、不复用
     */
    CoObjectId id{0};

    /**
     * 观测归类用的 kind
     */
    std::string kind;

    /**
     * 调用方给的可读名
     */
    std::string name;
};

/**
 * @brief 受管对象的最小身份接口。
 *
 * 只约束「能提供自己的身份」，不约束生命周期、执行域或网络能力；
 * 后者由更具体的接口分别描述。
 */
class ICoObject {
public:
    /**
     * @brief 虚析构，允许经接口指针销毁实现对象。
     */
    virtual ~ICoObject() = default;

    /**
     * @brief 当前身份快照；实现须在对象寿命内保持 id 不变。
     */
    virtual CoObjectInfo GetObjectInfo() const = 0;
};

}
