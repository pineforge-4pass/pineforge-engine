// Exercise the real adapter before testing its storage helper.
// The real-run witness at test-first commit 2f27bfa80 uses only the base API.
// Its expected red is completed same-id openings retained in OriginRoster;
// no declaration, sink option or module is missing. Executed red evidence
// and the final Release/Debug proof belong to CORE-TOP's lab.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;

int checks = 0;
int failures = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                 \
    if (!(condition)) {                                                       \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                           \
    }                                                                         \
} while (false)

// The existing ordering witnesses use this explicit-instantiation access
// pattern as well. No production fixture switch or public API is added.
template<class Tag, auto Member>
struct PrivateAccess {
    friend auto access(Tag) { return Member; }
};
struct CohortsTag { friend auto access(CohortsTag); };
template struct PrivateAccess<CohortsTag, &source::PineExecutionAdapter::cohorts_by_id_>;
struct PlacementTag { friend auto access(PlacementTag); };
template struct PrivateAccess<PlacementTag, &source::PineExecutionAdapter::placement_>;
struct SidesTag { friend auto access(SidesTag); };
template struct PrivateAccess<SidesTag, &source::PineExecutionAdapter::cohort_opened_on_side>;
struct PositionsTag { friend auto access(PositionsTag); };
template struct PrivateAccess<PositionsTag, &source::OriginRoster::positions_>;

std::vector<Bar> tape(int count) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1.0, kStart + i * 60000LL});
    return bars;
}

class CycleHost final : public source::PineStrategyHost {
public:
    CycleHost(int cycles, bool cancelled, bool recording)
        : cycles_(cycles), cancelled_(cancelled) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.process_orders_on_close = true;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        set_broker_state_hash_recording(recording);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i < cycles_ * 6) {
            if (i % 6 == 0)
                strategy_entry("entry", true, cancelled_ ? 50.0 : kNa, kNa, 1.0);
            if (i % 6 == 2) {
                if (cancelled_) strategy_cancel("entry");
                else strategy_close("entry");
            }
        }
        if (const auto* roster = origins()) {
            peak_members_ = std::max(peak_members_, roster->members().size());
        }
        // The latest-origin flag read must accept an empty retained window.
        if (i == cycles_ * 6 + 4) strategy_exit("late", "entry", 200.0, 50.0);
        if (i == cycles_ * 6 + 6) {
            const auto& cohorts = adapter_.*access(CohortsTag{});
            const auto found = cohorts.find("entry");
            CHECK(found != cohorts.end());
            if (found != cohorts.end()) {
                // First indexed side read after the prefix has gone: its
                // cursor starts at base, and historical sides still answer.
                CHECK((adapter_.*access(SidesTag{}))(found->second, true));
                CHECK(!(adapter_.*access(SidesTag{}))(found->second, false));
            }
        }
    }

    const source::OriginRoster* origins() const {
        const auto& cohorts = adapter_.*access(CohortsTag{});
        const auto found = cohorts.find("entry");
        return found == cohorts.end() ? nullptr : &found->second.origins;
    }
    std::size_t peak_members() const { return peak_members_; }

    void hash_host_extension(BrokerStateHashSink& sink) const override {
        source::PineStrategyHost::hash_host_extension(sink);
        sink.s("cr150-l19d/cycles");
        sink.i(cycles_); sink.i(cancelled_ ? 1 : 0); sink.u(peak_members_);
    }

private:
    int cycles_;
    bool cancelled_;
    std::size_t peak_members_ = 0;
};

void completed_cycles_drop_the_retained_prefix(int cycles, bool cancelled, bool recording) {
    CycleHost host(cycles, cancelled, recording);
    const auto bars = tape(cycles * 6 + 8);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    CHECK(host.last_error().empty());
    const auto* roster = host.origins();
    CHECK(roster != nullptr);
    if (!roster) return;
    // This is the base's first expected runtime failure: the historical
    // vector still has `cycles` members after all lots/requests ended.
    CHECK(roster->members().empty());
    CHECK(host.peak_members() <= 2);
    // Retirement must preserve the total accepted count used by hash_state.
    CHECK(roster->size() == static_cast<std::size_t>(cycles));
    CHECK(host.trade_count() == (cancelled ? 0 : cycles));
    if (!cancelled) {
        for (int i = 0; i < host.trade_count(); ++i) {
            const auto& trade = host.get_trade(i);
            CHECK(trade.entry_id == "entry");
            CHECK(trade.qty == 1.0);
            CHECK(trade.entry_price == 100.0);
            CHECK(trade.exit_price == 100.0);
        }
    }
}

class HeldOriginHost final : public source::PineStrategyHost {
public:
    static constexpr int kCancelled = 24;
    static constexpr int kCloseBar = kCancelled * 4 + 8;

    HeldOriginHost() {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.process_orders_on_close = true;
        config.pyramiding = 2;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) strategy_entry("entry", true, kNa, kNa, 1.0);
        if (i >= 4 && i < kCloseBar - 4) {
            if (i % 4 == 0) strategy_entry("entry", true, 50.0, kNa, 1.0);
            if (i % 4 == 2) strategy_cancel("entry");
        }
        if (i == kCloseBar) {
            const auto* roster = origins();
            CHECK(roster != nullptr);
            if (roster) {
                // Later cancellations cannot move a prefix past the first
                // origin while its lot remains in the kernel cohort.
                CHECK(roster->members().size() == roster->size());
                CHECK(roster->size() == static_cast<std::size_t>(kCancelled + 1));
            }
            CHECK(live_position_size() == 1.0);
            strategy_close_all();
        }
    }

    const source::OriginRoster* origins() const {
        const auto& cohorts = adapter_.*access(CohortsTag{});
        const auto found = cohorts.find("entry");
        return found == cohorts.end() ? nullptr : &found->second.origins;
    }
};

void an_open_oldest_lot_pins_the_prefix_until_release() {
    HeldOriginHost host;
    const auto bars = tape(HeldOriginHost::kCloseBar + 9);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    const auto* roster = host.origins();
    CHECK(roster != nullptr);
    if (!roster) return;
    CHECK(roster->members().empty());
    CHECK(roster->size() == static_cast<std::size_t>(HeldOriginHost::kCancelled + 1));
}

class ReferencePinHost final : public source::PineStrategyHost {
public:
    ReferencePinHost() {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.process_orders_on_close = true;
        config.pyramiding = 2;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) strategy_entry("entry", true, kNa, kNa, 1.0);
        if (i == 1) {
            strategy_entry("anchor", true, 50.0, kNa, 1.0);
            adapter_.flush_pending_entries();
        }
        if (i != 1 && i != 8) {
            if (i == 2) strategy_close("entry");
            if (i == 9) strategy_cancel("anchor");
            return;
        }
        auto& cohorts = adapter_.*access(CohortsTag{});
        const auto entry = cohorts.find("entry");
        CHECK(entry != cohorts.end());
        if (entry == cohorts.end()) return;
        auto& origins = entry->second.origins;
        CHECK(origins.members().size() == 1);
        if (origins.members().size() != 1) return;
        auto& placement = adapter_.*access(PlacementTag{});
        bool found_anchor = false;
        for (auto row : placement) {
            if (row.second.source_id != "anchor") continue;
            found_anchor = true;
            if (i == 1) {
                // A live row names an old origin even after that origin's
                // own placement and kernel cohort membership can go.
                row.second.bracket_origin = origins.members().front();
            } else {
                CHECK(!placement.contains(origins.members().front().incarnation));
                CHECK(entry->second.opened.empty());
                CHECK(entry->second.live_units_by_origin.empty());
                row.second.bracket_origin = {};
            }
        }
        CHECK(found_anchor);
        if (i == 8) {
            BrokerStateHashSink before, after;
            adapter_.hash_state(before);
            origins.retire_before(origins.size());
            adapter_.hash_state(after);
            // The entire source fold is byte-equivalent after retirement,
            // not just a separately computed digest of one helper.
            CHECK(before.h == after.h);
        }
    }
};

void a_live_placement_name_pins_a_released_origin_and_the_hash_stays_equal() {
    ReferencePinHost host;
    const auto bars = tape(20);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
}

native_order::RequestHandle handle(std::uint64_t incarnation, std::uint64_t run = 7) {
    return {{"cr150-l19d", run}, incarnation};
}

template<class Action>
void expect_invariant(Action action) {
    bool caught = false;
    try {
        action();
    } catch (const std::logic_error& error) {
        caught = true;
        CHECK(classify_run_failure(error).code == RunFailureCode::engine_invariant);
    }
    CHECK(caught);
}

std::uint64_t fold(std::uint64_t digest, std::uint64_t incarnation) {
    return (digest ^ incarnation) * 1099511628211ULL;
}

void global_positions_duplicates_and_invalid_reads() {
    source::OriginRoster roster;
    CHECK(roster.empty());
    CHECK(roster.base() == 0);
    expect_invariant([&] { (void)roster[0]; });
    expect_invariant([&] { (void)roster.back(); });
    const auto a = handle(10), b = handle(20), c = handle(10, 8), d = handle(30);
    const std::vector<native_order::RequestHandle> model{a, b, a, c, d};
    std::uint64_t digest = 1469598103934665603ULL;
    for (const auto& member : model) {
        roster.push_back(member);
        digest = fold(digest, member.incarnation);
    }
    std::vector<std::size_t> positions;
    roster.append_positions_of(a, positions);
    CHECK(positions == std::vector<std::size_t>({0, 2}));
    CHECK(*roster.positions_of(10) == std::vector<std::size_t>({0, 2, 3}));
    roster.retire_before(2);
    CHECK(roster.base() == 2);
    CHECK(roster.size() == model.size());
    CHECK(roster.digest() == digest);
    CHECK(roster.members() == std::vector<native_order::RequestHandle>({a, c, d}));
    CHECK(roster[2] == a);
    CHECK(roster.back() == d);
    CHECK(roster.positions_of(20) == nullptr);
    CHECK(*roster.positions_of(10) == std::vector<std::size_t>({2, 3}));
    positions.clear();
    roster.append_positions_of(a, positions);
    roster.append_positions_of(c, positions);
    CHECK(positions == std::vector<std::size_t>({2, 3}));
    CHECK(std::vector<native_order::RequestHandle>(roster.rbegin(), roster.rend())
          == std::vector<native_order::RequestHandle>({d, c, a}));
    expect_invariant([&] { (void)roster[1]; });
    expect_invariant([&] { (void)roster[5]; });
    expect_invariant([&] { roster.retire_before(1); });
    expect_invariant([&] { roster.retire_before(6); });
    roster.retire_before(5);
    CHECK(roster.base() == roster.size());
    CHECK(roster.members().empty());
    CHECK(!roster.empty()); // Logical history is still nonempty.
    CHECK(roster.begin() == roster.end());
    CHECK(roster.rbegin() == roster.rend());
    CHECK(roster.digest() == digest);
    CHECK(roster.positions_of(10) == nullptr);
    expect_invariant([&] { (void)roster.back(); });
    roster.push_back(b);
    CHECK(roster.size() == 6);
    CHECK(roster[5] == b);
    CHECK(*roster.positions_of(20) == std::vector<std::size_t>({5}));
    CHECK(roster.digest() == fold(digest, b.incarnation));
    const auto copied = roster;
    roster.retire_before(roster.size());
    CHECK(copied[5] == b);
    roster = source::OriginRoster{};
    CHECK(roster.empty());
    CHECK(roster.base() == 0);
    CHECK(roster.digest() == 1469598103934665603ULL);
}

void historical_capacities_and_lookup_nodes_are_released() {
    source::OriginRoster roster;
    constexpr std::size_t count = 8192;
    for (std::size_t i = 0; i < count; ++i) roster.push_back(handle(i + 1));
    const auto& index = roster.*access(PositionsTag{});
    const auto old_buckets = index.bucket_count();
    roster.retire_before(count - 2);
    CHECK(roster.members().size() == 2);
    CHECK(roster.members().capacity() <= 8);
    CHECK(index.size() == 2);
    CHECK(index.bucket_count() < old_buckets / 4);
    roster.retire_before(count);
    CHECK(roster.members().capacity() == 0);
    CHECK(index.empty());
    const source::OriginRoster empty;
    CHECK(index.bucket_count() == (empty.*access(PositionsTag{})).bucket_count());

    // Duplicate incarnations share an inner position vector; dropping its
    // prefix must free that vector's old peak too.
    const auto repeated = handle(42);
    for (std::size_t i = 0; i < count; ++i) roster.push_back(repeated);
    roster.retire_before(roster.size() - 1);
    const auto* positions = roster.positions_of(42);
    CHECK(positions != nullptr);
    if (positions) {
        CHECK(*positions == std::vector<std::size_t>({2 * count - 1}));
        CHECK(positions->capacity() <= 4);
    }
}

void operation_streams_match_an_append_only_reference() {
    for (std::uint64_t seed = 1; seed <= 32; ++seed) {
        std::uint64_t random = seed;
        const auto next = [&] {
            random ^= random << 13; random ^= random >> 7; random ^= random << 17;
            return random;
        };
        source::OriginRoster roster;
        std::vector<native_order::RequestHandle> model;
        std::size_t base = 0;
        std::uint64_t digest = 1469598103934665603ULL;
        for (int step = 0; step < 256; ++step) {
            if (next() % 4 != 0) {
                const auto incarnation = next() % 8;
                const auto member = handle(incarnation, 1 + next() % 2);
                roster.push_back(member);
                model.push_back(member);
                digest = fold(digest, member.incarnation);
            } else {
                base += static_cast<std::size_t>(next() % (model.size() - base + 1));
                roster.retire_before(base);
            }
            CHECK(roster.base() == base);
            CHECK(roster.size() == model.size());
            CHECK(roster.digest() == digest);
            CHECK(roster.members() == std::vector<native_order::RequestHandle>(
                model.begin() + static_cast<std::ptrdiff_t>(base), model.end()));
            for (std::uint64_t incarnation = 0; incarnation < 8; ++incarnation) {
                std::vector<std::size_t> expected;
                for (std::size_t i = base; i < model.size(); ++i)
                    if (model[i].incarnation == incarnation) expected.push_back(i);
                const auto* actual = roster.positions_of(incarnation);
                CHECK(expected.empty() ? actual == nullptr : actual && *actual == expected);
                for (std::uint64_t run = 1; run <= 2; ++run) {
                    const auto member = handle(incarnation, run);
                    expected.clear();
                    for (std::size_t i = base; i < model.size(); ++i)
                        if (model[i] == member) expected.push_back(i);
                    std::vector<std::size_t> actual_positions;
                    roster.append_positions_of(member, actual_positions);
                    CHECK(actual_positions == expected);
                }
            }
        }
    }
}

} // namespace

int main() {
    completed_cycles_drop_the_retained_prefix(10, false, false);
    completed_cycles_drop_the_retained_prefix(256, false, false);
    completed_cycles_drop_the_retained_prefix(96, false, true);
    completed_cycles_drop_the_retained_prefix(96, true, false);
    an_open_oldest_lot_pins_the_prefix_until_release();
    a_live_placement_name_pins_a_released_origin_and_the_hash_stays_equal();
    global_positions_duplicates_and_invalid_reads();
    historical_capacities_and_lookup_nodes_are_released();
    operation_streams_match_an_append_only_reference();
    std::printf("OriginRoster retirement: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
