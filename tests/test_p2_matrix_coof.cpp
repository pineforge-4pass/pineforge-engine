// A behavioural RED on frozen2: the real fill scheduler restores correct
// values there, but taking the checkpoint allocates a full matrix on the heap.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "p2_a5matudt_payload.hpp"
#include <pineforge/generic_matrix.hpp>
#include <pineforge/matrix.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <vector>
#include <malloc.h>
#if __has_include(<pineforge/detail/matrix_store.hpp>)
#include "p2_a5matudt_store.hpp"
#endif

namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
constexpr int initial_rows = 4096;
constexpr std::size_t checkpoint_bound = 4 * 4096;
std::size_t requested_bytes = 0;
void recorded(std::size_t bytes) { requested_bytes += bytes; }

struct Count {
    Count() {
        requested_bytes = 0;
        global_allocation::on_recorded = &recorded;
        global_allocation::recording = true;
    }
    ~Count() {
        global_allocation::recording = false;
        global_allocation::on_recorded = nullptr;
    }
};

void fold(std::uint64_t& hash, const void* bytes, std::size_t count) {
    const auto* p = static_cast<const unsigned char*>(bytes);
    for (std::size_t i = 0; i < count; ++i) {
        hash ^= p[i];
        hash *= UINT64_C(1099511628211);
    }
}

class Host final : public pineforge::source::PineStrategyHost {
public:
    pineforge::NumericMatrix numeric = pineforge::NumericMatrix::new_(initial_rows, 4);
    pineforge::NumericMatrix numeric_alias = numeric;
    pineforge::GenericMatrix<P2MatrixPayload> generic =
        pineforge::GenericMatrix<P2MatrixPayload>::new_(initial_rows, 1);
    pineforge::GenericMatrix<P2MatrixPayload> generic_alias = generic;
    int snapshots = 0, restores = 0, executions = 0;
    std::size_t largest_checkpoint = 0, largest_restore = 0;
    std::size_t largest_checkpoint_heap = 0;

    struct State {
        pineforge::NumericMatrix::Snapshot numeric;
        pineforge::GenericMatrix<P2MatrixPayload>::Snapshot generic;
        std::uint64_t digest;
    };
    std::optional<State> checkpoint;

    Host() {
        pineforge::source::PineStrategyConfig config{};
        config.calc_on_order_fills = true;
        configure_pine_strategy(config);
        for (int r = 0; r < initial_rows; ++r) {
            for (int c = 0; c < 4; ++c) numeric.set(r, c, r * 4.0 + c + 0.125);
            generic.set(r, 0, p2_matrix_payload(r));
        }
    }

    std::uint64_t digest() const {
        std::uint64_t hash = UINT64_C(14695981039346656037);
        const int rows = numeric.rows(), cols = numeric.columns();
        fold(hash, &rows, sizeof rows); fold(hash, &cols, sizeof cols);
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols; ++c) {
                const double value = numeric.get(r, c);
                fold(hash, &value, sizeof value);
            }
        const int count = generic.rows();
        fold(hash, &count, sizeof count);
        for (int r = 0; r < count; ++r) {
            const auto value = generic.get(r, 0);
            fold(hash, &value.bar, sizeof value.bar);
            fold(hash, &value.price, sizeof value.price);
            const auto label_bytes = value.label.size(), elements = value.history.size();
            fold(hash, &label_bytes, sizeof label_bytes);
            fold(hash, value.label.data(), label_bytes);
            fold(hash, &elements, sizeof elements);
            fold(hash, value.history.data(), elements * sizeof(double));
        }
        return hash;
    }

    void snapshot_script_state() override {
        ++snapshots;
        checkpoint.reset(); // freeing an old copy must not hide a new allocation
        const auto expected = digest();
        const auto before = ::mallinfo2();
        {
            Count window;
            checkpoint.emplace(State{numeric.snapshot(), generic.snapshot(), expected});
        }
        largest_checkpoint = std::max(largest_checkpoint, requested_bytes);
        const auto after = ::mallinfo2();
        const auto old_heap = static_cast<std::uint64_t>(before.uordblks) + before.hblkhd;
        const auto new_heap = static_cast<std::uint64_t>(after.uordblks) + after.hblkhd;
        largest_checkpoint_heap = std::max(largest_checkpoint_heap,
            static_cast<std::size_t>(new_heap > old_heap ? new_heap - old_heap : 0));
    }
    void restore_script_state() override {
        if (!checkpoint) return;
        ++restores;
        {
            Count window;
            numeric.restore(checkpoint->numeric);
            generic.restore(checkpoint->generic);
        }
        largest_restore = std::max(largest_restore, requested_bytes);
#if __has_include(<pineforge/detail/matrix_store.hpp>)
        pineforge::detail::matrix_store::current_runtime()->collect();
#endif
        require(digest() == checkpoint->digest, "calc-on-fill changed matrix bytes");
        require(numeric_alias.rows() == numeric.rows()
                    && numeric_alias.get(0, 0) == numeric.get(0, 0), "numeric alias rollback");
        require(generic_alias.rows() == generic.rows()
                    && p2_payload_equal(generic_alias.get(0, 0), generic.get(0, 0)),
                "UDT alias rollback");
    }
    void commit_script_state() override { snapshot_script_state(); }
    void on_source_bar(const pineforge::Bar&) override {
        ++executions;
        numeric.set(0, 0, numeric.get(0, 0) + 0.5);
        auto value = generic.get(0, 0);
        value.history.push_back(static_cast<double>(pine_bar_index()));
        value.label.append(1, 'x');
        generic.set(0, 0, value);
        // Exercise deletion, dimension rollback, and a live alias on each fill.
        if (numeric.rows() == initial_rows) numeric.remove_row(initial_rows - 1);
        if (generic.rows() == initial_rows) generic.remove_row(initial_rows - 1);
        if (pine_bar_index() % 4 == 0 && live_position_size() == 0) strategy_entry("L", true);
        if (pine_bar_index() % 4 == 2 && live_position_size() > 0) strategy_close("L");
#if __has_include(<pineforge/detail/matrix_store.hpp>)
        pineforge::detail::matrix_store::current_runtime()->collect();
#endif
    }
};
}

int main() {
    try {
        std::vector<pineforge::Bar> bars;
        for (int i = 0; i < 12; ++i)
            bars.push_back({100.0, 102.0, 99.0, 101.0, 1000.0,
                            INT64_C(1700000100000) + i * INT64_C(60000)});
#if __has_include(<pineforge/detail/matrix_store.hpp>)
        p2_a5::StoreFixture external_store;
#endif
        Host host;
#if __has_include(<pineforge/detail/matrix_store.hpp>)
        external_store.runtime.collect();
#endif
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false);
        require(host.last_error().empty(), host.last_error().c_str());
        require(host.restores > 0 && host.executions > static_cast<int>(bars.size()),
                "fill scheduler never restored script state");
        require(host.trade_count() > 0, "calc-on-fill fixture produced no trade");
        std::printf("P2-A5 matrix coof snapshots=%d restores=%d checkpoint=%zu restore=%zu"
                    " bound=%zu digest=%016" PRIx64 "\n",
                    host.snapshots, host.restores, host.largest_checkpoint,
                    host.largest_restore, checkpoint_bound, host.digest());
        require(host.largest_checkpoint <= checkpoint_bound, "snapshot heap copy scales with cells");
        require(host.largest_checkpoint_heap <= checkpoint_bound, "snapshot retains a full Eigen heap copy");
        require(host.largest_restore <= checkpoint_bound, "restore heap copy scales with cells");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "P2-A5 matrix coof: %s\n", error.what());
        return 1;
    }
}
