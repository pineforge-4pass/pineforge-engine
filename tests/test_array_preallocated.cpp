#include "global_allocation_replacement.hpp"
#include <pineforge/array.hpp>
#include "array_store_fixture.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace {
using namespace pineforge;
using array_test::require;

// Public Store calls only: count lifecycle requests without inspecting the
// merged ScratchStore cache or object/chunk records.
class ObservedStore final : public detail::o1::Store {
    detail::o1::Store& inner_;
public:
    std::size_t creates = 0;
    std::size_t resizes = 0;
    explicit ObservedStore(detail::o1::Store& inner) : inner_(inner) {}
    detail::o1::Id create(std::uint64_t n, std::uint64_t a) override {
        ++creates; return inner_.create(n, a);
    }
    void release(detail::o1::Id id) override { inner_.release(id); }
    bool alive(detail::o1::Id id) const override { return inner_.alive(id); }
    std::uint64_t size(detail::o1::Id id) const override { return inner_.size(id); }
    bool resize(detail::o1::Id id, std::uint64_t n) override {
        ++resizes; return inner_.resize(id, n);
    }
    std::size_t read(detail::o1::Id id, std::uint64_t offset, void* p, std::size_t n) const override {
        return inner_.read(id, offset, p, n);
    }
    std::size_t write(detail::o1::Id id, std::uint64_t offset, const void* p, std::size_t n) override {
        return inner_.write(id, offset, p, n);
    }
    std::unique_ptr<detail::o1::Tx> begin() override { return inner_.begin(); }
    bool join_active_tx() override { return inner_.join_active_tx(); }
    void abandon_joined_tx() noexcept override { inner_.abandon_joined_tx(); }
    std::unique_ptr<detail::o1::Tx> begin_cleanup() override { return inner_.begin_cleanup(); }
    void note_cleanup_skipped(detail::o1::CleanupSkip reason) noexcept override {
        inner_.note_cleanup_skipped(reason);
    }
    detail::o1::Stats stats() const override { return inner_.stats(); }
    std::uint64_t rollback_epoch() const noexcept override { return inner_.rollback_epoch(); }
};
} // namespace

int main() {
    try {
        array_test::StoreFixture fixture;
        ObservedStore store(*fixture.store);
        // Known before bar 0. Thirty-two payload pages plus one cell exceed
        // even the merged store's cache + work pages + fixed metadata:
        // 16385 * 8 = 131080 > (8 + 6 + 1 + 2) * 4096 + 16 + 1024.
        constexpr std::int64_t cardinality = 16385;
        auto values = ScriptArray<double>::preallocated(store, cardinality);
        const auto creates = store.creates;
        const auto resizes = store.resizes;
        const auto allocations = global_allocation::allocations;
        constexpr std::int64_t bars = 16;
        for (std::int64_t bar = 0; bar < bars; ++bar) {
            values.clear();
            for (std::int64_t i = 0; i < cardinality; ++i)
                values.push(static_cast<double>(bar * cardinality + i));
            values.set_i(0, -static_cast<double>(bar));
            require(values.get_i(0) == -static_cast<double>(bar), "prepared index write changed");
            require(values.last() == static_cast<double>((bar + 1) * cardinality - 1),
                    "prepared tail write changed");
        }
        require(global_allocation::allocations == allocations,
                "proved array allocated C++ heap on a bar");
        require(store.creates == creates && store.resizes == resizes,
                "proved array allocated/resized an object on a bar");
        // A proof is preparation, not a new runtime capacity limit.
        constexpr std::int64_t beyond_window = 100000;
        values.clear();
        for (std::int64_t i = 0; i < beyond_window; ++i) values.push(static_cast<double>(i));
        require(values.size() == beyond_window && values.first() == 0.0 &&
                values.last() == static_cast<double>(beyond_window - 1),
                "exceeding a prepared proof refused or truncated");
        std::printf("array prepared: %lld bars x %lld cells, zero heap/lifecycle allocations; growth=%lld\n",
                    static_cast<long long>(bars), static_cast<long long>(cardinality),
                    static_cast<long long>(beyond_window));
        return 0;
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "array prepared: %s\n", failure.what());
        return 1;
    }
}
