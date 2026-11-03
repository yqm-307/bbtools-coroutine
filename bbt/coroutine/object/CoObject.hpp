/* 对象身份与实际寿命观测；执行域限制由独立的 CoRuntimeOnly 表达。 */
#pragma once

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <bbt/coroutine/object/ManagerBase.hpp>
#include <bbt/coroutine/object/interface/ICoObject.hpp>

namespace bbt::coroutine {

/* 未初始化时抛 std::logic_error；id 进程内严格递增、不复用，溢出抛 std::overflow_error。 */
CoObjectInfo CreateObjectInfo(std::string kind, std::string name);

class CoObject : public ICoObject, public MemberBase<CoObjectId, CoObject> {
public:
    explicit CoObject(std::string kind, std::string name);
    ~CoObject() noexcept override = default;

    CoObjectInfo GetObjectInfo() const override { return GetMemberObjectInfo(); }

protected:
    /* 默认构造的派生对象由 creator 在交付 owner 前初始化唯一身份。 */
    CoObject() = default;
};

using CoObjectManager = ManagerBase<CoObjectId, CoObject>;

/* 默认观测 manager 不拥有成员，保持进程寿命，避免静态退出期注销访问悬空状态。 */
std::shared_ptr<CoObjectManager> GetCoObjectManager();

template <class T, class Creator>
auto MakeCoObject(std::string kind, std::string name, Creator&& creator)
    -> decltype(std::forward<Creator>(creator)()) {
    static_assert(std::is_base_of_v<CoObject, T>, "受管对象必须继承 CoObject");
    using Owner = decltype(std::forward<Creator>(creator)());
    using Object = std::remove_reference_t<decltype(*std::declval<Owner&>())>;
    static_assert(std::is_base_of_v<T, Object>, "creator 返回的类型必须匹配 T");
    if constexpr (std::is_base_of_v<CoRuntimeOnly, Object>)
        RequireCoroutineContext();
    return GetCoObjectManager()->Create(std::move(kind), std::move(name), std::forward<Creator>(creator));
}

} // namespace bbt::coroutine
