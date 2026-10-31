#include "p2_a5strmap_store_fixture.hpp"
#include <pineforge/map.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {
namespace o1 = pineforge::detail::o1;
using o1::CleanupSkip;
using o1::Id;
using o1::Stats;
using o1::Store;
using o1::Tx;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// scope() is inherited: its TxScope must call this wrapper's public hooks,
// which must be able to reach the inner Store's hooks through a Store&.
class ForwardingStore : public Store {
public:
    explicit ForwardingStore(Store& inner) noexcept : inner_(inner) {}
    ~ForwardingStore() override = default;

    Id create(std::uint64_t bytes, std::uint64_t align) override {
        return inner_.create(bytes, align);
    }
    void release(Id id) override { inner_.release(id); }
    bool alive(Id id) const override { return inner_.alive(id); }
    std::uint64_t size(Id id) const override { return inner_.size(id); }
    bool resize(Id id, std::uint64_t bytes) override {
        return inner_.resize(id, bytes);
    }
    std::size_t read(Id id, std::uint64_t off, void* dst, std::size_t n) const override {
        return inner_.read(id, off, dst, n);
    }
    std::size_t write(Id id, std::uint64_t off, const void* src, std::size_t n) override {
        return inner_.write(id, off, src, n);
    }
    std::unique_ptr<Tx> begin() override { return inner_.begin(); }
    bool join_active_tx() override { return inner_.join_active_tx(); }
    void abandon_joined_tx() noexcept override { inner_.abandon_joined_tx(); }
    std::unique_ptr<Tx> begin_cleanup() override { return inner_.begin_cleanup(); }
    void note_cleanup_skipped(CleanupSkip reason) noexcept override {
        inner_.note_cleanup_skipped(reason);
    }
    Stats stats() const override { return inner_.stats(); }
    std::uint64_t rollback_epoch() const noexcept override { return inner_.rollback_epoch(); }

private:
    Store& inner_;
};

// Deliberately broken wrapper: every virtual still forwards except joining.
class SilentNoJoinStore final : public ForwardingStore {
public:
    using ForwardingStore::ForwardingStore;
    bool join_active_tx() override { return false; }
};

struct FullCallerPool {
    explicit FullCallerPool(Store& store) {
        for (;;) {
            try {
                auto next = store.begin(); // Explicit caller reservation.
                require(explicit_begins < handles.size(),
                        "wrapper caller pool exceeded its declared size");
                handles[explicit_begins++] = std::move(next);
            } catch (const o1::TxPoolExhausted&) {
                break;
            }
        }
        require(explicit_begins == o1::kMaxTxHandles,
                "wrapper did not fill the declared caller pool");
    }

    void commit() {
        for (std::size_t index = 1; index < explicit_begins; ++index)
            handles[index]->commit();
        handles.front()->commit();
    }

    std::array<std::unique_ptr<Tx>, o1::kMaxTxHandles> handles{};
    std::size_t explicit_begins = 0;
};

template <std::size_t Depth>
struct MapTree {
    using Child = typename MapTree<Depth - 1>::Map;
    using Map = pineforge::PineMap<int, Child>;

    static Map build() {
        auto node = Map::new_();
        auto child = MapTree<Depth - 1>::build();
        (void)node.put(static_cast<int>(Depth), child);
        return node;
    }
    static std::size_t depth(const Map& node) {
        require(!node.is_na() && node.size() == 1 &&
                    node.contains(static_cast<int>(Depth)),
                "wrapper tree lost a map level");
        return 1 + MapTree<Depth - 1>::depth(node.get(static_cast<int>(Depth)));
    }
};

template <>
struct MapTree<1> {
    using Map = pineforge::PineMap<int, int>;
    static Map build() {
        auto node = Map::new_();
        (void)node.put(7, 97);
        return node;
    }
    static std::size_t depth(const Map& node) {
        require(!node.is_na() && node.size() == 1 && node.get(7) == 97,
                "wrapper tree lost its leaf payload");
        return 1;
    }
};

template <class Wrapper>
void wrapper_depth_six(bool negative_control) {
    a5_fixture::Session session; // Real disk backing and StoreImpl.
    auto& inner = *session.store;
    Wrapper wrapper(inner);
    const auto baseline = inner.stats().bytes_live;
    auto holder = std::make_shared<pineforge::detail::MapStoreHolder>(wrapper);
    pineforge::detail::MapStoreScope map_scope(holder);
    FullCallerPool pool(wrapper); // Every explicit begin() goes through the wrapper.
    require(inner.stats().max_live_handles == pool.explicit_begins,
            "wrapper setup did not account for every caller handle");
    constexpr std::size_t levels = 6;
    bool exhausted = false;
    try {
        auto tree = MapTree<levels>::build();
        require(MapTree<levels>::depth(tree) == levels,
                "wrapper traversal did not reach six real maps");
        require(inner.stats().bytes_live > baseline,
                "wrapper tree never allocated real store objects");
    } catch (const o1::TxPoolExhausted&) {
        exhausted = true;
    }
    const auto stats = inner.stats(); // Observe the real pool, through no wrapper.
    if (negative_control) {
        require(exhausted == true,
                "silent no-join wrapper escaped the pool-pressure control");
    } else {
        require(!exhausted, "forwarding wrapper exhausted the full caller pool");
        require(stats.max_live_handles == pool.explicit_begins,
                "forwarding wrapper consumed a handle beyond explicit begin calls");
    }
    require(stats.bytes_live == baseline,
            "wrapper tree destruction leaked store bytes");
    require(stats.cleanup_skipped_rollback_only == 0 && stats.cleanup_failed == 0,
            "wrapper tree cleanup was skipped or failed");
    pool.commit();
    require(inner.stats().bytes_live == baseline,
            "wrapper tree commit changed the empty-store baseline");
    std::printf("store wrapper: %s depth=%zu explicit=%zu high_water=%llu exhausted=%s\n",
                negative_control ? "no-join control" : "forwarding",
                levels, pool.explicit_begins,
                static_cast<unsigned long long>(stats.max_live_handles),
                exhausted ? "yes" : "no");
}

void wrapper_abandon(bool explicit_rollback) {
    a5_fixture::Session session;
    auto& inner = *session.store;
    ForwardingStore wrapper(inner);
    auto holder = std::make_shared<pineforge::detail::MapStoreHolder>(wrapper);
    pineforge::detail::MapStoreScope map_scope(holder);
    using Leaf = MapTree<1>::Map;
    auto map = std::make_unique<Leaf>(MapTree<1>::build());
    const auto before = inner.stats();
    const auto before_epoch = wrapper.rollback_epoch();
    {
        auto outer = wrapper.begin();
        {
            auto joined = wrapper.scope();
            if (explicit_rollback) joined.rollback();
        } // Otherwise destruction must forward abandonment through the wrapper.
        bool refused = false;
        try {
            auto scope = wrapper.scope();
            scope.commit();
        } catch (const o1::RollbackOnly&) {
            refused = true;
        }
        require(refused, "wrapper abandonment did not poison the inner transaction");
        map.reset(); // Also exercises forwarding begin_cleanup and the reason.
        require(inner.stats().cleanup_skipped_rollback_only == 1 &&
                    inner.stats().cleanup_failed == 0,
                "wrapper did not forward the benign cleanup diagnostic");
        require(wrapper.rollback_epoch() == before_epoch,
                "joined abandonment advanced the epoch before restoring bytes");
        outer->rollback();
    }
    require(wrapper.rollback_epoch() == before_epoch + 1 &&
                inner.rollback_epoch() == before_epoch + 1,
            "wrapper did not forward the completed outer rollback epoch");
    const auto stats = wrapper.stats();
    require(stats.bytes_live == before.bytes_live &&
                stats.objects_live == before.objects_live &&
                stats.cleanup_skipped_rollback_only == 1 && stats.cleanup_failed == 0,
            "wrapper rollback lost retained state or sticky diagnostics");
    require(stats.cleanup_skipped_rollback_only == inner.stats().cleanup_skipped_rollback_only &&
                stats.cleanup_failed == inner.stats().cleanup_failed,
            "wrapper stats did not forward cleanup diagnostics");
    std::printf("store wrapper: %s forwards abandonment and cleanup reason\n",
                explicit_rollback ? "rollback" : "scope exit");
}
} // namespace

int main() {
    try {
        wrapper_depth_six<ForwardingStore>(false);
        wrapper_depth_six<SilentNoJoinStore>(true);
        wrapper_abandon(false);
        wrapper_abandon(true);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "store wrapper FAIL: %s\n", error.what());
        return 1;
    }
}
