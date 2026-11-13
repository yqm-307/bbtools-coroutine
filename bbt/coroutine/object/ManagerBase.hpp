/**
 * @file ManagerBase.hpp
 * @brief 对象创建提交、非 owning 索引与成员析构通知。
 */
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include <bbt/coroutine/object/CoObjectStats.hpp>
#include <bbt/coroutine/object/CoRuntimeOnly.hpp>
#include <bbt/coroutine/object/interface/ICoObject.hpp>

namespace bbt::coroutine {

/**
 * @brief 分配一个对象身份（id/kind/name）。
 *
 * runtime 未初始化时抛 `std::logic_error`；id 进程内严格递增、不复用，
 * 溢出抛 `std::overflow_error`。
 */
CoObjectInfo CreateObjectInfo(std::string kind, std::string name);

template <class TKey, class TMember> class ManagerBase;

namespace detail {

template <class TMember> struct CoMemberEntry {
    CoObjectInfo info;
    std::weak_ptr<TMember> owner;
    bool shared{false};
};

template <class T> struct IsSharedCoOwner : std::false_type {};

template <class T> struct IsSharedCoOwner<std::shared_ptr<T>> : std::true_type {};

}

/**
 * @brief 被 Manager 登记的对象侧基类：持有身份、key 与注销入口。
 *
 * 成员不拥有 Manager，只持 `weak_ptr`；析构路径只做计数结算与索引注销，
 * 不调用业务代码、不挂起、不抛异常。构造/拷贝/移动都不对用户开放。
 */
template <class TKey, class TMember> class MemberBase {
    friend class ManagerBase<TKey, TMember>;
    using EntryMap = std::map<TKey, detail::CoMemberEntry<TMember>>;

public:
    using ManagerType = ManagerBase<TKey, TMember>;

    MemberBase(const MemberBase&) = delete;
    MemberBase& operator=(const MemberBase&) = delete;
    MemberBase(MemberBase&&) = delete;
    MemberBase& operator=(MemberBase&&) = delete;

    /**
     * @brief 登记时使用的 key（默认即对象身份 id）；未提交成员无 key。
     */
    const TKey& GetMemberId() const;

    /**
     * @brief 身份快照（id/kind/name），只读成员自身字段，不访问 Manager。
     */
    CoObjectInfo GetMemberObjectInfo() const;

protected:
    MemberBase() = default;
    explicit MemberBase(CoObjectInfo info);
    ~MemberBase() noexcept;

    std::shared_ptr<ManagerType> GetManager() const noexcept { return m_manager.lock(); }

private:
    CoObjectInfo m_info;
    std::optional<TKey> m_key;
    std::weak_ptr<ManagerType> m_manager;
    std::shared_ptr<detail::CoObjectCounters> m_stats;
    detail::CoObjectCounters* m_kindStats{nullptr};
    typename EntryMap::iterator m_entry;
    bool m_committed{false};
};

/**
 * @brief 唯一 creator 提交点与成员索引。
 *
 * 索引不拥有成员：`shared_ptr` 成员只存 `weak_ptr`，`unique_ptr` 成员连弱
 * 引用都不存。Manager 必须先于成员销毁时不留下悬空注销入口——每个成员
 * 自己持一份记账状态，Manager 已析构时只结算计数。
 */
template <class TKey, class TMember>
class ManagerBase : public std::enable_shared_from_this<ManagerBase<TKey, TMember>> {
    friend class MemberBase<TKey, TMember>;
    using Member = MemberBase<TKey, TMember>;
    using EntryMap = std::map<TKey, detail::CoMemberEntry<TMember>>;

public:
    using KeyType = TKey;

    /**
     * @brief 空索引；须由 `shared_ptr` 持有（`Create` 依赖 `shared_from_this`）。
     */
    ManagerBase() = default;
    ManagerBase(const ManagerBase&) = delete;
    ManagerBase& operator=(const ManagerBase&) = delete;

    /**
     * @brief 立即调用一次 creator，登记其成员后把 owner/deleter 原样返回。
     * Manager 须由 `shared_ptr` 持有；creator 交付的对象在返回前不得并发访问或登记。继承
     * `CoRuntimeOnly` 时先做协程域检查（不在协程内抛 `std::logic_error`，不构造对象）；失败均抛异常且不提交计数。
     */
    template <class Creator>
    auto Create(const TKey& key, std::string kind, std::string name, Creator&& creator)
        -> decltype(std::forward<Creator>(creator)());

    /**
     * @brief 以对象身份为 key 的便捷入口，不再分配第二个身份。
     * @tparam Creator 返回合法对象 owner 的零参可调用类型
     * @param kind 对象统计归类
     * @param name 对象可读名
     * @param creator 零参 owner 创建器
     * @return 登记后原样移交的 owner
     */
    template <class Creator>
    auto Create(std::string kind, std::string name, Creator&& creator) -> decltype(std::forward<Creator>(creator)());

    /**
     * @brief 本 Manager 自己那份计数快照；与 `GetCoObjectStats(kind)` 各自独立记账。
     */
    CoObjectStats GetStats() const noexcept;

    /**
     * @brief 按 key 取身份快照。
     *
     * 共享成员只 lock 一次并在锁外释放短暂 owner；独占成员只读身份元数据。
     * 不调用派生对象，也不把观测引用带出查询。
     * @param key 成员索引键
     * @return 成员存在时返回身份快照，否则返回 `std::nullopt`
     */
    std::optional<CoObjectInfo> GetObjectInfo(const TKey& key) const;

private:
    template <class Creator>
    auto _Create(const TKey* key, std::string kind, std::string name, Creator&& creator)
        -> decltype(std::forward<Creator>(creator)());

    void _OnMemberDestroy(typename EntryMap::iterator entry) noexcept;

    mutable std::mutex m_mutex;
    EntryMap m_members;
    std::shared_ptr<detail::CoObjectCounters> m_stats{std::make_shared<detail::CoObjectCounters>()};
};

}

#include <bbt/coroutine/object/__TManagerBase.hpp>
