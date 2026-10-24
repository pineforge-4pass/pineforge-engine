// CR-150 L19l/L19m: real reversals with storage release/eviction on and off.
// The script/tape are the Flip shape in test_adapter_lookup_index_witness.
// No pinned values: the reference keeps cohort rows and empty capacity, as
// the old implementation did, while placement retirement stays enabled.
#include <pineforge/detail/adapter_exposure_storage.hpp>
#include <pineforge/detail/disk_id_table.hpp>

// Load every dependency before the narrowly scoped predicate substitution.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace reversal_storage_fixture {
struct Probe {
    bool keep_capacity = false;
    bool keep_cohorts = false;
    std::size_t spills = 0;
    std::size_t retained_rows = 0;
    std::size_t releases = 0;
    std::size_t clears = 0;
};
inline Probe* active = nullptr;

// Test-local copies of the old storage operations: erase kept an empty
// allocation, clear retained capacity, and an empty cohort node stayed in
// the map. None changes the placement-retirement override or logical state.
template<class Container> void old_release_empty(Container&) {}
template<class Container> void old_clear(Container& container) { container.clear(); }
inline bool old_retain_cohort() noexcept { return false; }

inline bool allow_spill() noexcept {
    if (active && active->keep_cohorts) {
        ++active->retained_rows;
        return old_retain_cohort();
    }
    if (active) ++active->spills;
    return true;
}
} // namespace reversal_storage_fixture

// Only the predicate call in the storage template is substituted. Both
// modes keep the exact production class layout, public access and codec;
// the reference simply declines eviction, retaining the old map's nodes.
// Counts are taken after the real pin/spillable checks, before write/erase.
#define spillable() spillable() && ::reversal_storage_fixture::allow_spill()
#include <pineforge/detail/disk_resident_id_table.hpp>
#undef spillable

#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/source/pine_placement_retention.hpp"

namespace pineforge::detail {
template<class Container>
void reversal_fixture_release_empty(Container& container) {
    if (reversal_storage_fixture::active)
        ++reversal_storage_fixture::active->releases;
    if (reversal_storage_fixture::active
        && reversal_storage_fixture::active->keep_capacity)
        reversal_storage_fixture::old_release_empty(container);
    else
        release_empty_exposure_capacity(container);
}
template<class Container>
void reversal_fixture_clear(Container& container) {
    if (reversal_storage_fixture::active)
        ++reversal_storage_fixture::active->clears;
    if (reversal_storage_fixture::active
        && reversal_storage_fixture::active->keep_capacity)
        reversal_storage_fixture::old_clear(container);
    else
        clear_exposure_storage(container);
}
} // namespace pineforge::detail

// As in test_adapter_growth_storage, compile the real adapter with only its
// storage operations interposed. Include its hash TU too: every operation
// that can evict a cohort, including a hash read, uses this test's predicate.
// These definitions satisfy both objects before the static archive search.
#define release_empty_exposure_capacity reversal_fixture_release_empty
#define clear_exposure_storage reversal_fixture_clear
#include "../src/source/pine_adapter.cpp"
#undef clear_exposure_storage
#undef release_empty_exposure_capacity
#include "../src/source/pine_state_hash.cpp"

namespace reversal_storage_fixture {
using namespace pineforge;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;
constexpr int kBars = 96;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool same_bits(double left, double right) {
    return std::memcmp(&left, &right, sizeof left) == 0;
}

int triangle(int index, int period) {
    const int phase = index % (2 * period);
    return phase < period ? phase : 2 * period - phase;
}
double price(int index) {
    return 0.25 * (400 + 2 * triangle(index, 9)
                  + 3 * triangle(index, 23) - triangle(index, 4));
}
std::vector<Bar> tape() {
    std::vector<Bar> bars;
    for (int i = 0; i < kBars; ++i) {
        const double open = price(i), close = price(i + 1);
        bars.push_back({open, std::max(open, close) + 0.5,
                        std::min(open, close) - 0.5, close, 1.0,
                        kStart + static_cast<std::int64_t>(i) * 60000});
    }
    return bars;
}

class ReversalHost final : public source::PineStrategyHost {
public:
    explicit ReversalHost(bool recording) {
        source::PineStrategyConfig config;
        config.initial_capital = 100000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 2;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 0.5;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        set_broker_state_hash_recording(recording);
    }
    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        const double position = live_position_size();
        if (position != 0.0) {
            const int side = position > 0.0 ? 1 : -1;
            if (last_side_ != 0 && side != last_side_) ++reversals;
            last_side_ = side;
        }
        if (i % 6 == 1) strategy_entry("L", true);
        if (i % 6 == 4) strategy_entry("S", false);
        if (i % 12 == 2 && position > 0.0) strategy_entry("L2", true);
        if (i % 18 == 9) strategy_entry("B", true);
        if (i % 18 == 15) strategy_entry("B", false);
        if (position > 0.0)
            strategy_exit("xl", "L", bar.close + 2.0, bar.close - 2.0);
        if (position < 0.0)
            strategy_exit("xs", "S", bar.close - 2.0, bar.close + 2.0,
                          kNa, kNa, kNa, 100.0, {}, 1.0);
        if (i % 10 == 7) strategy_close("L", {}, 1.0);
        if (i % 14 == 11) strategy_order("o", true, 1.0);
        if (i % 14 == 12) strategy_order("o", false, 1.0);
        reads.push_back(broker_state_hash());
    }
    const std::vector<std::uint64_t>& hashes() const { return broker_state_hashes_; }
    std::vector<std::uint64_t> reads;
    std::size_t reversals = 0;
private:
    int last_side_ = 0;
};

struct Result {
    std::vector<Trade> trades;
    std::vector<std::uint64_t> hashes, reads;
    std::uint64_t final_hash = 0;
    std::uint64_t erased_rows = 0;
    std::size_t reversals = 0;
    Probe operations;
};

Result run(bool keep_capacity, bool keep_cohorts, bool magnifier, bool recording) {
    require(!source::detail::retain_retired_rows(),
            "the witness requires ordinary placement retirement in every mode");
    const auto bars = tape();
    ReversalHost host(recording);
    Probe probe;
    probe.keep_capacity = keep_capacity;
    probe.keep_cohorts = keep_cohorts;
    const auto erased_before = source::detail::retired_rows_erased();
    active = &probe;
    try {
        if (magnifier)
            host.run(bars.data(), static_cast<int>(bars.size()), "", "", true, 4,
                     MagnifierDistribution::ENDPOINTS);
        else
            host.run(bars.data(), static_cast<int>(bars.size()));
        require(host.last_error().empty(), "live reversal run completes");
        Result result;
        for (int i = 0; i < host.trade_count(); ++i)
            result.trades.push_back(host.get_trade(i));
        result.hashes = host.hashes();
        result.reads = host.reads;
        result.final_hash = host.broker_state_hash();
        result.erased_rows = source::detail::retired_rows_erased() - erased_before;
        result.reversals = host.reversals;
        result.operations = probe;
        active = nullptr;
        return result;
    } catch (...) {
        active = nullptr;
        throw;
    }
}

void compare_trade(const Trade& a, const Trade& b) {
    require(a.entry_time == b.entry_time && a.exit_time == b.exit_time
            && a.entry_bar_index == b.entry_bar_index && a.exit_bar_index == b.exit_bar_index
            && a.entry_id == b.entry_id && a.exit_id == b.exit_id
            && a.entry_comment == b.entry_comment && a.exit_comment == b.exit_comment
            && a.entry_incarnation == b.entry_incarnation && a.is_long == b.is_long
            && a.open_at_end == b.open_at_end && a.exit_from_bracket == b.exit_from_bracket
            && a.close_cause == b.close_cause,
            "reversal trade identities and metadata match the old storage operations");
    require(same_bits(a.entry_price, b.entry_price) && same_bits(a.exit_price, b.exit_price)
            && same_bits(a.qty, b.qty) && same_bits(a.pnl, b.pnl)
            && same_bits(a.pnl_pct, b.pnl_pct) && same_bits(a.commission, b.commission)
            && same_bits(a.max_runup, b.max_runup) && same_bits(a.max_drawdown, b.max_drawdown),
            "reversal trade doubles match the old storage operations bit for bit");
}

void compare(const Result& actual, const Result& old, bool magnifier, bool recording) {
    const auto mismatch = [](const auto& left, const auto& right) {
        std::size_t i = 0;
        for (; i < left.size() && i < right.size() && left[i] == right[i]; ++i) {}
        return i;
    };
    if (actual.reads != old.reads || actual.hashes != old.hashes
        || actual.final_hash != old.final_hash || actual.trades.size() != old.trades.size()) {
        std::fprintf(stderr,
            "reversal storage: keep_capacity=%d keep_cohorts=%d magnifier=%d recording=%d "
            "first read=%zu first row=%zu trades=%zu/%zu final=%llu/%llu "
            "spills=%zu retained=%zu\n",
            actual.operations.keep_capacity ? 1 : 0, actual.operations.keep_cohorts ? 1 : 0,
            magnifier ? 1 : 0, recording ? 1 : 0,
            mismatch(actual.reads, old.reads), mismatch(actual.hashes, old.hashes),
            actual.trades.size(), old.trades.size(),
            static_cast<unsigned long long>(actual.final_hash),
            static_cast<unsigned long long>(old.final_hash),
            actual.operations.spills, actual.operations.retained_rows);
    }
    require(actual.reads == old.reads
            && actual.reads.size() == static_cast<std::size_t>(kBars),
            "every reversal callback hash matches the old storage operations");
    require(actual.hashes == old.hashes && (recording ? !actual.hashes.empty()
                                                   : actual.hashes.empty()),
            "every recorded reversal hash matches the old storage operations");
    require(actual.final_hash == old.final_hash, "final reversal hashes match bit for bit");
    require(actual.trades.size() == old.trades.size(), "reversal trade populations match");
    for (std::size_t i = 0; i < actual.trades.size(); ++i)
        compare_trade(actual.trades[i], old.trades[i]);
    require(actual.erased_rows > 0 && actual.erased_rows == old.erased_rows,
            "placement retirement runs identically with spilling enabled and disabled");
    require(actual.reversals > 1 && actual.reversals == old.reversals,
            "both runs reverse the live position repeatedly");
    require(actual.operations.releases > 0 && actual.operations.clears > 0,
            "reversals exercise both production exposure storage operations");
    if (actual.operations.keep_cohorts)
        require(actual.operations.spills == 0 && actual.operations.retained_rows > 0,
                "the old cohort retention operation disables eligible evictions");
    else
        require(actual.operations.spills > 0 && actual.operations.retained_rows == 0,
                "the production run really evicts eligible cohort rows");
}

void reversal_storage_matches_the_old_operations() {
    for (const bool magnifier : {false, true}) {
        for (const bool recording : {false, true}) {
            const auto old = run(true, true, magnifier, recording);
            require(old.operations.spills == 0 && old.operations.retained_rows > 0
                    && old.operations.releases > 0 && old.operations.clears > 0,
                    "the reference exercises the old retain-storage operations");
            // Isolate L19m and L19l-A as well as their combined live path.
            compare(run(false, true, magnifier, recording), old, magnifier, recording);
            compare(run(true, false, magnifier, recording), old, magnifier, recording);
            compare(run(false, false, magnifier, recording), old, magnifier, recording);
        }
    }
}
} // namespace reversal_storage_fixture

int main() {
    try {
        reversal_storage_fixture::reversal_storage_matches_the_old_operations();
        std::puts("test_adapter_reversal_storage: reversal trades and hashes preserve old storage");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_adapter_reversal_storage: %s\n", error.what());
        return 1;
    }
}
