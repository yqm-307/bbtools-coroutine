/* 唯一 creator 提交与成员析构通知；索引不拥有成员，析构通知不调用业务代码。 */
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

CoObjectInfo CreateObjectInfo(std::string kind, std::string name);

template <class TKey, class TMember>
class ManagerBase;

namespace detail {

template <class TMember>
struct CoMemberEntry {
    CoObjectInfo info;
    std::weak_ptr<TMember> owner;
    bool shared{false};
};

template <class T>
struct IsSharedCoOwner : std::false_type {};

template <class T>
struct IsSharedCoOwner<std::shared_ptr<T>> : std::true_type {};

} // namespace detail

template <class TKey, class TMember>
class MemberBase {
    friend class ManagerBase<TKey, TMember>;
    using EntryMap = std::map<TKey, detail::CoMemberEntry<TMember>>;

public:
    using ManagerType = ManagerBase<TKey, TMember>;

    MemberBase(const MemberBase&) = delete;
    MemberBase& operator=(const MemberBase&) = delete;
    MemberBase(MemberBase&&) = delete;
    MemberBase& operator=(MemberBase&&) = delete;

    const TKey& GetMemberId() const { return m_key.value(); }
    CoObjectInfo GetMemberObjectInfo() const { return m_info; }

protected:
    MemberBase() = default;
    explicit MemberBase(CoObjectInfo info) : m_info(std::move(info)) {}
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

template <class TKey, class TMember>
class ManagerBase : public std::enable_shared_from_this<ManagerBase<TKey, TMember>> {
    friend class MemberBase<TKey, TMember>;
    using Member = MemberBase<TKey, TMember>;
    using EntryMap = std::map<TKey, detail::CoMemberEntry<TMember>>;

public:
    using KeyType = TKey;

    ManagerBase() = default;
    ManagerBase(const ManagerBase&) = delete;
    ManagerBase& operator=(const ManagerBase&) = delete;

    /* Manager 须由 shared_ptr 持有；creator 立即调用一次，原 owner/deleter 原样返回。
     * creator 交付完整、尚未发布的对象；交付 owner 前不得并发访问或登记该对象。
     * key 冲突、构造或登记失败均抛异常，且不提交 created/destroyed。 */
    template <class Creator>
    auto Create(const TKey& key, std::string kind, std::string name, Creator&& creator)
        -> decltype(std::forward<Creator>(creator)()) {
        return _Create(&key, std::move(kind), std::move(name), std::forward<Creator>(creator));
    }

    /* 以对象身份为 key 的便捷入口，不再分配第二个身份。 */
    template <class Creator>
    auto Create(std::string kind, std::string name, Creator&& creator)
        -> decltype(std::forward<Creator>(creator)()) {
        static_assert(std::is_same_v<TKey, CoObjectId>, "自动 key 仅适用于 CoObjectId");
        return _Create(nullptr, std::move(kind), std::move(name), std::forward<Creator>(creator));
    }

    CoObjectStats GetStats() const noexcept { return m_stats->GetStats(); }

    /* 共享成员只 lock 一次并在锁外释放短暂 owner；独占成员只读身份元数据。
     * 不调用派生对象，也不把观测引用带出查询。 */
    std::optional<CoObjectInfo> GetObjectInfo(const TKey& key) const {
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
        -> decltype(std::forward<Creator>(creator)()) {
        using Owner = decltype(std::forward<Creator>(creator)());
        using Object = std::remove_reference_t<decltype(*std::declval<Owner&>())>;
        static_assert(std::is_base_of_v<TMember, Object>, "creator 对象必须派生自 TMember");
        static_assert(std::is_base_of_v<Member, TMember>, "TMember 必须继承对应 MemberBase");
        static_assert(!std::is_reference_v<Owner>, "creator 必须按值移交 owner");
        static_assert(std::is_nothrow_move_constructible_v<Owner>, "提交后 owner 移交不得抛异常");
        static_assert(std::is_nothrow_destructible_v<TKey>, "析构注销不得调用可抛出的 key 析构");

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

    void _OnMemberDestroy(typename EntryMap::iterator entry) noexcept {
        std::lock_guard<std::mutex> lock(m_mutex);
        // 用稳定 iterator 注销，不比较/复制用户 key，不访问已析构的派生对象。
        m_members.erase(entry);
    }

    mutable std::mutex m_mutex;
    EntryMap m_members;
    std::shared_ptr<detail::CoObjectCounters> m_stats{std::make_shared<detail::CoObjectCounters>()};
};

template <class TKey, class TMember>
MemberBase<TKey, TMember>::~MemberBase() noexcept {
    if (!m_committed)
        return;
    m_stats->OnDestroy();
    m_kindStats->OnDestroy();
    if (auto manager = m_manager.lock())
        manager->_OnMemberDestroy(m_entry);
}

} // namespace bbt::coroutine
