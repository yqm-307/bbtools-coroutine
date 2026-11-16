/**
 * @file __TManagerBase.hpp
 * @brief MemberBase / ManagerBase 的模板实现，由 ManagerBase.hpp 末尾包含。
 */
#pragma once
#include <bbt/coroutine/object/ManagerBase.hpp>

namespace bbt::coroutine {

template <class TKey, class TMember> const TKey& MemberBase<TKey, TMember>::GetMemberId() const
{
    return m_key.value();
}

template <class TKey, class TMember> CoObjectInfo MemberBase<TKey, TMember>::GetMemberObjectInfo() const
{
    return m_info;
}

template <class TKey, class TMember> MemberBase<TKey, TMember>::MemberBase(CoObjectInfo info) : m_info(std::move(info))
{
}

template <class TKey, class TMember>
template <class Creator>
auto ManagerBase<TKey, TMember>::Create(const TKey& key, std::string kind, std::string name, Creator&& creator)
    -> decltype(std::forward<Creator>(creator)())
{
    return _Create(&key, std::move(kind), std::move(name), std::forward<Creator>(creator));
}

template <class TKey, class TMember>
template <class Creator>
auto ManagerBase<TKey, TMember>::Create(std::string kind, std::string name, Creator&& creator)
    -> decltype(std::forward<Creator>(creator)())
{
    static_assert(std::is_same_v<TKey, CoObjectId>, "ManagerBase: automatic key requires TKey == CoObjectId");
    return _Create(nullptr, std::move(kind), std::move(name), std::forward<Creator>(creator));
}

template <class TKey, class TMember> CoObjectStats ManagerBase<TKey, TMember>::GetStats() const noexcept
{
    return m_stats->GetStats();
}

template <class TKey, class TMember>
std::optional<CoObjectInfo> ManagerBase<TKey, TMember>::GetObjectInfo(const TKey& key) const
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

template <class TKey, class TMember>
template <class Creator>
auto ManagerBase<TKey, TMember>::_Create(const TKey* key, std::string kind, std::string name, Creator&& creator)
    -> decltype(std::forward<Creator>(creator)())
{
    using Owner = decltype(std::forward<Creator>(creator)());
    using Object = std::remove_reference_t<decltype(*std::declval<Owner&>())>;
    static_assert(std::is_base_of_v<TMember, Object>, "ManagerBase: creator object must derive from TMember");
    static_assert(std::is_base_of_v<Member, TMember>, "ManagerBase: TMember must derive from its MemberBase");
    static_assert(!std::is_reference_v<Owner>, "ManagerBase: creator must return the owner by value");
    static_assert(std::is_nothrow_move_constructible_v<Owner>, "ManagerBase: owner move after commit must not throw");
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

        /**
             * 登记之后只执行不抛出的提交；失败对象从未启用析构通知。
             */
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

template <class TKey, class TMember>
void ManagerBase<TKey, TMember>::_OnMemberDestroy(typename EntryMap::iterator entry) noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);

    /**
         * 用稳定 iterator 注销，不比较/复制用户 key，不访问已析构的派生对象。
         */
    m_members.erase(entry);
}

template <class TKey, class TMember> MemberBase<TKey, TMember>::~MemberBase() noexcept
{
    if (!m_committed)
        return;
    m_stats->OnDestroy();
    m_kindStats->OnDestroy();
    if (auto manager = m_manager.lock())
        manager->_OnMemberDestroy(m_entry);
}

}
