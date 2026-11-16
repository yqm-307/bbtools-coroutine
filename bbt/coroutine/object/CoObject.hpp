/**
 * @file CoObject.hpp
 * @brief 对象身份与实际寿命观测；执行域限制由独立的 CoRuntimeOnly 表达。
 */
#pragma once

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <bbt/coroutine/object/ManagerBase.hpp>
#include <bbt/coroutine/object/interface/ICoObject.hpp>

namespace bbt::coroutine {

/**
 * @brief 分配对象身份（id/kind/name）。
 *
 * runtime 未初始化时抛 `std::logic_error`；id 进程内严格递增、不复用，
 * 溢出抛 `std::overflow_error`。
 */
CoObjectInfo CreateObjectInfo(std::string kind, std::string name);

/**
 * @brief 带身份与创建观测的运行时对象基类。
 *
 * 同时实现 `ICoObject`（身份查询）与 `MemberBase`（登记/注销）。构造与析构
 * 都不要求协程域；需要限定域的派生类型另外继承 `CoRuntimeOnly`。
 */
class CoObject : public ICoObject, public MemberBase<CoObjectId, CoObject> {
public:
    /**
     * @brief 分配新身份并登记到默认 Manager；runtime 未初始化时抛 `std::logic_error`。
     */
    explicit CoObject(std::string kind, std::string name);

    /**
     * @brief 析构并结算对象实际销毁；不抛出异常。
     */
    ~CoObject() noexcept override = default;

    /**
     * @brief 当前身份快照（id/kind/name）；id 在对象寿命内不变。
     */
    CoObjectInfo GetObjectInfo() const override { return GetMemberObjectInfo(); }

protected:
    /**
     * @brief 默认构造：派生对象由 creator 在交付 owner 前初始化唯一身份。
     */
    CoObject() = default;
};

using CoObjectManager = ManagerBase<CoObjectId, CoObject>;

/**
 * @brief 进程级默认观测 Manager。
 *
 * 不拥有成员，保持进程寿命，避免静态退出期的注销访问悬空状态。
 */
std::shared_ptr<CoObjectManager> GetCoObjectManager();

/**
 * @brief 经默认 `CoObjectManager` 创建一个受管对象。
 *
 * 薄转发：不重新分配 ID、不重复登记，creator 的 owner/deleter 原样返回。
 * creator 返回的对象类型继承 `CoRuntimeOnly` 时，在调用 creator 之前先检查
 * 协程域。
 */
template <class T, class Creator>
auto MakeCoObject(std::string kind, std::string name, Creator&& creator) -> decltype(std::forward<Creator>(creator)())
{
    static_assert(std::is_base_of_v<CoObject, T>, "MakeCoObject: managed type must derive from CoObject");
    using Owner = decltype(std::forward<Creator>(creator)());
    using Object = std::remove_reference_t<decltype(*std::declval<Owner&>())>;
    static_assert(std::is_base_of_v<T, Object>, "MakeCoObject: creator return type must match T");
    if constexpr (std::is_base_of_v<CoRuntimeOnly, Object>)
        RequireCoroutineContext();
    return GetCoObjectManager()->Create(std::move(kind), std::move(name), std::forward<Creator>(creator));
}

}
