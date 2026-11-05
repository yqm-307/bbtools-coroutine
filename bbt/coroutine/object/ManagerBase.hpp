/** @file ManagerBase.hpp
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

} // namespace detail

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

    /// @brief 登记时使用的 key（默认即对象身份 id）；未提交成员无 key。
    const TKey& GetMemberId() const
    {
        return m_key.value();
    }
    /// @brief 身份快照（id/kind/name），只读成员自身字段，不访问 Manager。
    CoObjectInfo GetMemberObjectInfo() const
    {
        return m_info;
    }

protected:
    MemberBase() = default;
    explicit MemberBase(CoObjectInfo info) : m_info(std::move(info))
    {
    }
    ~MemberBase() noexcept;

    std::shared_ptr<ManagerType> GetManager() const noexcept
    {
        return m_manager.lock();
    }

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

    /// @brief 空索引；须由 `shared_ptr` 持有（`Create` 依赖 `shared_from_this`）。
    ManagerBase() = default;
    ManagerBase(const ManagerBase&) = delete;
    ManagerBase& operator=(const ManagerBase&) = delete;

    /**
     * @brief 立即调用一次 creator，登记其成员后把 owner/deleter 原样返回。
     *
     * Manager 须由 `shared_ptr` 持有。creator 交付完整但尚未发布的对象；
     * 交付 owner 之前不得并发访问或登记该对象。key 冲突、构造或登记失败都
     * 抛异常，且不提交 created/destroyed。
     * @tparam Creator 返回合法对象 owner 的零参可调用类型
     * @param key 成员索引键
     * @param kind 对象统计归类
     * @param name 对象可读名
     * @param creator 零参 owner 创建器
     * @return 登记后原样移交的 owner
     */
    template <class Creator>
    auto Create(const TKey& key, std::string kind, std::string name, Creator&& creator)
        -> decltype(std::forward<Creator>(creator)())
    {
        return _Create(&key, std::move(kind), std::move(name), std::forward<Creator>(creator));
    }

    /**
     * @brief 以对象身份为 key 的便捷入口，不再分配第二个身份。
     * @tparam Creator 返回合法对象 owner 的零参可调用类型
     * @param kind 对象统计归类
     * @param name 对象可读名
     * @param creator 零参 owner 创建器
     * @return 登记后原样移交的 owner
     */
    template <class Creator>
    auto Create(std::string kind, std::string name, Creator&& creator) -> decltype(std::forward<Creator>(creator)())
    {
        static_assert(std::is_same_v<TKey, CoObjectId>, "ManagerBase: automatic key requires TKey == CoObjectId");
        return _Create(nullptr, std::move(kind), std::move(name), std::forward<Creator>(creator));
    }

    /// @brief 本 Manager 自己那份计数快照；与 `GetCoObjectStats(kind)` 各自独立记账。
    CoObjectStats GetStats() const noexcept
    {
        return m_stats->GetStats();
    }

    /**
     * @brief 按 key 取身份快照。
     *
     * 共享成员只 lock 一次并在锁外释放短暂 owner；独占成员只读身份元数据。
     * 不调用派生对象，也不把观测引用带出查询。
     * @param key 成员索引键
     * @return 成员存在时返回身份快照，否则返回 `std::nullopt`
     */
    std::optional<CoObjectInfo> GetObjectInfo(const TKey& key) const
    {
        std::shared_ptr<TMember> owner;
        std::optional<CoObjectInfo> info;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto it = m_members.find(key);
            if (it == m_members.end())
                return std::nullopt;
            if (it->second.shared) {
                owner = it->second.owner.lock();
                if (!owner)
                    return std::nullopt;
            }
            info = it->second.info;
        }
        return info;
    }

private:
    template <class Creator>
    auto _Create(const TKey* key, std::string kind, std::string name, Creator&& creator)
        -> decltype(std::forward<Creator>(creator)())
    {
        using Owner = decltype(std::forward<Creator>(creator)());
        using Object = std::remove_reference_t<decltype(*std::declval<Owner&>())>;
        static_assert(std::is_base_of_v<TMember, Object>, "ManagerBase: creator object must derive from TMember");
        static_assert(std::is_base_of_v<Member, TMember>, "ManagerBase: TMember must derive from its MemberBase");
        static_assert(!std::is_reference_v<Owner>, "ManagerBase: creator must return the owner by value");
        static_assert(std::is_nothrow_move_constructible_v<Owner>,
                      "ManagerBase: owner move after commit must not throw");
        static_assert(std::is_nothrow_destructible_v<TKey>, "ManagerBase: member key destruction must not throw");

        if constexpr (std::is_base_of_v<CoRuntimeOnly, Object>)
            RequireCoroutineContext();
        auto manager = this->shared_from_this();
        auto owner = std::forward<Creator>(creator)();
        if (!owner)
            throw std::invalid_argument("ManagerBase: creator returned an empty owner");

        auto& member = static_cast<Member&>(*owner);
        if (member.m_committed)
            throw std::logic_error("ManagerBase: member already registered");

        auto info = member.m_info;
        if constexpr (std::is_base_of_v<ICoObject, Object>) {
            if (info.id == 0)
                info = (*owner).GetObjectInfo();
        }
        if (info.id == 0)
            info = CreateObjectInfo(std::move(kind), std::move(name));
        auto& kind_stats = detail::GetOrCreateCoObjectCounters(info.kind);
        if (key) {
            member.m_key.emplace(*key);
        } else {
            if constexpr (std::is_same_v<TKey, CoObjectId>)
                member.m_key.emplace(info.id);
        }

        detail::CoMemberEntry<TMember> entry{info, {}, false};
        if constexpr (detail::IsSharedCoOwner<Owner>::value) {
            entry.owner = owner;
            entry.shared = true;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto inserted = m_members.emplace(*member.m_key, std::move(entry));
            if (!inserted.second)
                throw std::logic_error("ManagerBase: duplicate member key");

            // 登记之后只执行不抛出的提交；失败对象从未启用析构通知。
            member.m_info = std::move(info);
            member.m_manager = manager;
            member.m_stats = m_stats;
            member.m_kindStats = &kind_stats;
            member.m_entry = inserted.first;
            member.m_committed = true;
            m_stats->OnCreate();
            kind_stats.OnCreate();
        }
        return owner;
    }

    void _OnMemberDestroy(typename EntryMap::iterator entry) noexcept
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // 用稳定 iterator 注销，不比较/复制用户 key，不访问已析构的派生对象。
        m_members.erase(entry);
    }

    mutable std::mutex m_mutex;
    EntryMap m_members;
    std::shared_ptr<detail::CoObjectCounters> m_stats{std::make_shared<detail::CoObjectCounters>()};
};

template <class TKey, class TMember> MemberBase<TKey, TMember>::~MemberBase() noexcept
{
    if (!m_committed)
        return;
    m_stats->OnDestroy();
    m_kindStats->OnDestroy();
    if (auto manager = m_manager.lock())
        manager->_OnMemberDestroy(m_entry);
}

} // namespace bbt::coroutine
