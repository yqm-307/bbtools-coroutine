#include <bbt/coroutine/object/CoObject.hpp>
#include <bbt/coroutine/object/MemberBase.hpp>

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MAIN
#include <boost/test/included/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <bbt/coroutine/coroutine.hpp>

using namespace bbt::coroutine;

namespace {

struct RuntimeFixture {
    RuntimeFixture() {
        static std::once_flag once;
        std::call_once(once, []() {
            g_bbt_coroutine_config->m_cfg_static_thread_num = 2;
            g_scheduler->Start();
        });
    }
};

struct Probe {
    std::atomic<int> destroyed{0};
    std::atomic<bool> native{false};
};

class Object : public CoObject {
public:
    explicit Object(std::shared_ptr<Probe> probe = {}) : m_probe(std::move(probe)) {}
    Object(std::string kind, std::string name) : CoObject(std::move(kind), std::move(name)) {}
    ~Object() noexcept override {
        if (m_probe) {
            m_probe->native.store(!IsInCoroutineContext());
            m_probe->destroyed.fetch_add(1);
        }
    }
    void Close() noexcept { m_closed = true; }
    bool IsClosed() const noexcept { return m_closed; }

private:
    std::shared_ptr<Probe> m_probe;
    bool m_closed{false};
};

struct OnlyObject : Object, CoRuntimeOnly {
    using Object::Object;
};

struct ThrowingObject : CoObject {
    ThrowingObject() : CoObject("objects.constructor-failure", "partial") {
        throw std::runtime_error("construction failure");
    }
};

class RestrictedObject final : public CoObject {
public:
    struct Deleter {
        void operator()(RestrictedObject* object) const noexcept { delete object; }
    };
    using UPtr = std::unique_ptr<RestrictedObject, Deleter>;
    static UPtr Create() { return UPtr(new RestrictedObject); }

private:
    RestrictedObject() = default;
    ~RestrictedObject() noexcept override = default;
};

static_assert(!std::is_default_constructible_v<RestrictedObject>);
static_assert(!std::is_destructible_v<RestrictedObject>);
static_assert(!std::is_base_of_v<CoRuntimeOnly, CoObject>);
static_assert(!std::is_copy_constructible_v<Object>);
static_assert(std::is_nothrow_destructible_v<Object>);

struct NativeMember : MemberBase<int, NativeMember> {};

struct ThrowKey {
    explicit ThrowKey(std::shared_ptr<int> budget) : copies_left(std::move(budget)) {}
    ThrowKey(const ThrowKey& other) : copies_left(other.copies_left) {
        if ((*copies_left)-- == 0)
            throw std::bad_alloc();
    }
    bool operator<(const ThrowKey&) const noexcept { return false; }
    std::shared_ptr<int> copies_left;
};

struct ThrowMember : MemberBase<ThrowKey, ThrowMember> {
    explicit ThrowMember(std::shared_ptr<Probe> probe) : m_probe(std::move(probe)) {}
    ~ThrowMember() noexcept { m_probe->destroyed.fetch_add(1); }
    std::shared_ptr<Probe> m_probe;
};

void CheckStats(CoObjectStats stats, std::uint64_t created, std::uint64_t destroyed,
                std::uint64_t alive, std::uint64_t peak) {
    BOOST_CHECK_EQUAL(stats.created_total, created);
    BOOST_CHECK_EQUAL(stats.destroyed_total, destroyed);
    BOOST_CHECK_EQUAL(stats.alive, alive);
    BOOST_CHECK_EQUAL(stats.peak_alive, peak);
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(RuntimeObjects, RuntimeFixture)

BOOST_AUTO_TEST_CASE(identity_default_creator_close_and_owner_move) {
    const std::string kind = "objects.unique";
    auto object = MakeCoObject<Object>(kind, "first", []() { return std::make_unique<Object>(); });
    const auto info = object->GetObjectInfo();
    BOOST_CHECK_NE(info.id, 0u);
    BOOST_CHECK_EQUAL(info.kind, kind);
    BOOST_CHECK_EQUAL(info.name, "first");
    BOOST_CHECK_EQUAL(object->GetMemberId(), info.id);
    auto snapshot = GetCoObjectManager()->GetObjectInfo(info.id);
    BOOST_REQUIRE(snapshot.has_value());
    snapshot->name = "local copy";
    BOOST_CHECK_EQUAL(object->GetObjectInfo().name, "first");
    CheckStats(GetCoObjectStats(kind), 1, 0, 1, 1);

    object->Close();
    auto moved = std::move(object);
    BOOST_CHECK(!object);
    BOOST_CHECK(moved->IsClosed());
    CheckStats(GetCoObjectStats(kind), 1, 0, 1, 1);
    moved.reset();
    BOOST_CHECK(!GetCoObjectManager()->GetObjectInfo(info.id).has_value());
    CheckStats(GetCoObjectStats(kind), 1, 1, 0, 1);
}

BOOST_AUTO_TEST_CASE(existing_identity_is_not_reallocated_or_renamed) {
    auto manager = std::make_shared<CoObjectManager>();
    const auto before = CreateObjectInfo("identity.probe", "before");
    auto object = manager->Create(CoObjectId{41}, "ignored-kind", "ignored-name", []() {
        return std::make_unique<Object>("objects.existing", "original");
    });
    const auto after = CreateObjectInfo("identity.probe", "after");
    const auto info = object->GetObjectInfo();
    BOOST_CHECK_EQUAL(info.id, before.id + 1);
    BOOST_CHECK_EQUAL(after.id, info.id + 1);
    BOOST_CHECK_EQUAL(info.kind, "objects.existing");
    BOOST_CHECK_EQUAL(info.name, "original");
    BOOST_CHECK_EQUAL(object->GetMemberId(), 41u);
    CheckStats(manager->GetStats(), 1, 0, 1, 1);
    object.reset();
    CheckStats(manager->GetStats(), 1, 1, 0, 1);
}

BOOST_AUTO_TEST_CASE(shared_index_is_weak_and_last_owner_notifies_once) {
    auto manager = std::make_shared<CoObjectManager>();
    auto probe = std::make_shared<Probe>();
    auto object = manager->Create("objects.shared", "shared", [probe]() {
        return std::make_shared<Object>(probe);
    });
    const auto id = object->GetObjectInfo().id;
    std::weak_ptr<Object> weak = object;
    auto other = object;
    BOOST_CHECK_EQUAL(object.use_count(), 2);
    BOOST_REQUIRE(manager->GetObjectInfo(id).has_value());
    BOOST_CHECK_EQUAL(object.use_count(), 2);
    object.reset();
    CheckStats(manager->GetStats(), 1, 0, 1, 1);
    other.reset();
    BOOST_CHECK(!weak.lock());
    BOOST_CHECK(!manager->GetObjectInfo(id).has_value());
    BOOST_CHECK_EQUAL(probe->destroyed.load(), 1);
    BOOST_CHECK(probe->native.load());
    CheckStats(manager->GetStats(), 1, 1, 0, 1);
    CheckStats(GetCoObjectStats("objects.shared"), 1, 1, 0, 1);
}

BOOST_AUTO_TEST_CASE(manager_can_die_before_shared_and_unique_members) {
    auto manager = std::make_shared<CoObjectManager>();
    std::weak_ptr<CoObjectManager> weak_manager = manager;
    auto probe = std::make_shared<Probe>();
    auto unique = manager->Create("objects.orphan", "unique", [probe]() {
        return std::make_unique<Object>(probe);
    });
    auto shared = manager->Create("objects.orphan", "shared", [probe]() {
        return std::make_shared<Object>(probe);
    });
    manager.reset();
    BOOST_CHECK(!weak_manager.lock());
    CheckStats(GetCoObjectStats("objects.orphan"), 2, 0, 2, 2);
    unique.reset();
    shared.reset();
    BOOST_CHECK_EQUAL(probe->destroyed.load(), 2);
    BOOST_CHECK(probe->native.load());
    CheckStats(GetCoObjectStats("objects.orphan"), 2, 2, 0, 2);
}

BOOST_AUTO_TEST_CASE(restricted_constructor_and_deleter_are_preserved) {
    auto object = MakeCoObject<RestrictedObject>("objects.restricted", "private", []() {
        return RestrictedObject::Create();
    });
    static_assert(std::is_same_v<decltype(object), RestrictedObject::UPtr>);
    BOOST_CHECK_NE(object->GetObjectInfo().id, 0u);
    CheckStats(GetCoObjectStats("objects.restricted"), 1, 0, 1, 1);
    object.reset();
    CheckStats(GetCoObjectStats("objects.restricted"), 1, 1, 0, 1);
}

BOOST_AUTO_TEST_CASE(generic_manager_member_uses_the_same_accounting) {
    auto manager = std::make_shared<ManagerBase<int, NativeMember>>();
    auto member = manager->Create(7, "objects.generic", "native", []() {
        return std::make_unique<NativeMember>();
    });
    const auto info = member->GetMemberObjectInfo();
    BOOST_CHECK_NE(info.id, 0u);
    BOOST_CHECK_EQUAL(member->GetMemberId(), 7);
    BOOST_REQUIRE(manager->GetObjectInfo(7).has_value());
    BOOST_CHECK_EQUAL(manager->GetObjectInfo(7)->id, info.id);
    member.reset();
    BOOST_CHECK(!manager->GetObjectInfo(7).has_value());
    CheckStats(manager->GetStats(), 1, 1, 0, 1);
    CheckStats(GetCoObjectStats("objects.generic"), 1, 1, 0, 1);
}

BOOST_AUTO_TEST_CASE(creator_throw_and_empty_owner_do_not_commit) {
    auto manager = std::make_shared<CoObjectManager>();
    int calls = 0;
    auto throwing = [&calls]() -> std::unique_ptr<Object> {
        ++calls;
        throw std::runtime_error("creator failure");
    };
    BOOST_CHECK_THROW(manager->Create("objects.creator-failure", "throw", throwing), std::runtime_error);
    BOOST_CHECK_EQUAL(calls, 1);
    BOOST_CHECK_THROW(manager->Create("objects.creator-failure", "empty", []() {
        return std::unique_ptr<Object>{};
    }),
                      std::invalid_argument);
    CheckStats(manager->GetStats(), 0, 0, 0, 0);
    CheckStats(GetCoObjectStats("objects.creator-failure"), 0, 0, 0, 0);

    BOOST_CHECK_THROW(MakeCoObject<ThrowingObject>("objects.constructor-failure", "partial", []() {
                          return std::make_unique<ThrowingObject>();
                      }),
                      std::runtime_error);
    CheckStats(GetCoObjectStats("objects.constructor-failure"), 0, 0, 0, 0);
}

BOOST_AUTO_TEST_CASE(exception_after_owner_delivery_counts_actual_destruction) {
    BOOST_CHECK_THROW(([]() {
                          auto object = MakeCoObject<Object>("objects.post-create-failure", "delivered", []() {
                              return std::make_unique<Object>();
                          });
                          throw std::runtime_error("failure after creator commit");
                      })(),
                      std::runtime_error);
    CheckStats(GetCoObjectStats("objects.post-create-failure"), 1, 1, 0, 1);
}

BOOST_AUTO_TEST_CASE(duplicate_key_destroys_uncommitted_object_without_destroy_accounting) {
    auto manager = std::make_shared<CoObjectManager>();
    const CoObjectId key = 9;
    auto first = manager->Create(key, "objects.duplicate", "first", []() {
        return std::make_shared<Object>();
    });
    auto probe = std::make_shared<Probe>();
    int calls = 0;
    BOOST_CHECK_THROW(manager->Create(key, "objects.duplicate", "second", [probe, &calls]() {
        ++calls;
        return std::make_unique<Object>(probe);
    }),
                      std::logic_error);
    BOOST_CHECK_EQUAL(calls, 1);
    BOOST_CHECK_EQUAL(probe->destroyed.load(), 1);
    BOOST_REQUIRE(manager->GetObjectInfo(key).has_value());
    BOOST_CHECK_EQUAL(manager->GetObjectInfo(key)->name, "first");
    CheckStats(manager->GetStats(), 1, 0, 1, 1);
    CheckStats(GetCoObjectStats("objects.duplicate"), 1, 0, 1, 1);

    BOOST_CHECK_THROW(manager->Create(CoObjectId{10}, "objects.duplicate", "repeat", [first]() {
        return first;
    }),
                      std::logic_error);
    BOOST_CHECK(!manager->GetObjectInfo(10).has_value());
    CheckStats(manager->GetStats(), 1, 0, 1, 1);
    first.reset();
    CheckStats(manager->GetStats(), 1, 1, 0, 1);
}

BOOST_AUTO_TEST_CASE(index_insertion_exception_does_not_arm_member_or_commit) {
    auto manager = std::make_shared<ManagerBase<ThrowKey, ThrowMember>>();
    auto probe = std::make_shared<Probe>();
    // 首次复制初始化 member key，第二次复制由 map 插入触发异常。
    ThrowKey key(std::make_shared<int>(1));
    BOOST_CHECK_THROW(manager->Create(key, "objects.index-failure", "failed", [probe]() {
        return std::make_unique<ThrowMember>(probe);
    }),
                      std::bad_alloc);
    BOOST_CHECK_EQUAL(probe->destroyed.load(), 1);
    CheckStats(manager->GetStats(), 0, 0, 0, 0);
    CheckStats(GetCoObjectStats("objects.index-failure"), 0, 0, 0, 0);
    BOOST_CHECK(!manager->GetObjectInfo(key).has_value());
}

BOOST_AUTO_TEST_CASE(co_only_gate_precedes_creator_and_destruction_is_native_safe) {
    int calls = 0;
    auto creator = [&calls]() {
        ++calls;
        return std::make_unique<OnlyObject>();
    };
    auto manager = std::make_shared<CoObjectManager>();
    BOOST_CHECK_THROW(manager->Create("objects.co-only", "native", creator), std::logic_error);
    BOOST_CHECK_THROW(MakeCoObject<OnlyObject>("objects.co-only", "native", creator), std::logic_error);
    BOOST_CHECK_EQUAL(calls, 0);
    CheckStats(manager->GetStats(), 0, 0, 0, 0);
    CheckStats(GetCoObjectStats("objects.co-only"), 0, 0, 0, 0);

    auto probe = std::make_shared<Probe>();
    auto done = std::make_shared<std::promise<std::unique_ptr<OnlyObject>>>();
    auto future = done->get_future();
    g_scheduler->RegistCoroutineTask(128 * 1024, [done, probe]() {
        try {
            done->set_value(MakeCoObject<OnlyObject>("objects.co-only", "co", [probe]() {
                return std::make_unique<OnlyObject>(probe);
            }));
        } catch (...) {
            done->set_exception(std::current_exception());
        }
    });
    BOOST_REQUIRE(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    auto object = future.get();
    CheckStats(GetCoObjectStats("objects.co-only"), 1, 0, 1, 1);
    BOOST_CHECK(!IsInCoroutineContext());
    object.reset();
    BOOST_CHECK_EQUAL(probe->destroyed.load(), 1);
    BOOST_CHECK(probe->native.load());
    CheckStats(GetCoObjectStats("objects.co-only"), 1, 1, 0, 1);
}

BOOST_AUTO_TEST_CASE(concurrent_create_snapshot_and_destroy_preserve_totals_and_peak) {
    constexpr int threads = 4;
    constexpr int batch = 16;
    const std::string kind = "objects.concurrent";
    auto manager = std::make_shared<CoObjectManager>();
    struct Barrier {
        std::mutex mutex;
        std::condition_variable cv;
        int ready{0};
        bool release{false};
        std::exception_ptr error;
    };
    auto barrier = std::make_shared<Barrier>();
    std::vector<std::thread> workers;
    for (int thread = 0; thread < threads; ++thread) {
        workers.emplace_back([manager, barrier, kind]() {
            std::vector<std::shared_ptr<Object>> objects;
            try {
                for (int i = 0; i < batch; ++i) {
                    auto object = manager->Create(kind, "parallel", []() {
                        return std::make_shared<Object>();
                    });
                    (void)manager->GetObjectInfo(object->GetMemberId());
                    (void)GetCoObjectStats(kind);
                    objects.push_back(std::move(object));
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(barrier->mutex);
                barrier->error = std::current_exception();
            }
            std::unique_lock<std::mutex> lock(barrier->mutex);
            ++barrier->ready;
            barrier->cv.notify_all();
            barrier->cv.wait(lock, [&]() { return barrier->release; });
        });
    }
    bool ready = false;
    CoObjectStats held;
    {
        std::unique_lock<std::mutex> lock(barrier->mutex);
        ready = barrier->cv.wait_for(lock, std::chrono::seconds(5), [&]() {
            return barrier->ready == threads;
        });
        held = GetCoObjectStats(kind);
        barrier->release = true;
    }
    barrier->cv.notify_all();
    for (auto& worker : workers)
        worker.join();
    BOOST_REQUIRE(ready);
    if (barrier->error)
        std::rethrow_exception(barrier->error);
    CheckStats(held, threads * batch, 0, threads * batch, threads * batch);
    CheckStats(manager->GetStats(), threads * batch, threads * batch, 0, threads * batch);
    CheckStats(GetCoObjectStats(kind), threads * batch, threads * batch, 0, threads * batch);
}

BOOST_AUTO_TEST_CASE(unknown_kind_has_an_empty_native_snapshot) {
    BOOST_CHECK(!IsInCoroutineContext());
    CheckStats(GetCoObjectStats("objects.never-created"), 0, 0, 0, 0);
}

BOOST_AUTO_TEST_SUITE_END()
