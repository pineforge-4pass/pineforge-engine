// The limit on order ids active at once (placed and not yet closed or
// cancelled) is the run's own U, the cohort-id maximum of its scratch manifest.
//
// The former fixed limit of 4,096 stopped a run that the default manifest
// (U = 65,536) allowed far more ids. The first row places and holds 4,097
// distinct ids under the default manifest, then closes all of them; the closed
// trades and their profit are compared with values computed from the script's
// own price tape. The other rows show what does not move: a run under the former
// limit gives byte-identical closed trades on the disk and the bounded backends,
// a manifest whose layout cannot be represented is refused before the first
// bar, and a host's explicit maxima are honored exactly.
#include <pineforge/detail/scratch_defaults.hpp>
#include <pineforge/detail/scratch_slab.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
using Adapter = pineforge::source::PineExecutionAdapter;
namespace detail = pineforge::detail;

// The same explicit-instantiation access seam as test_cohort_for_live_limit_new_id.cpp:
// no changed class definition or production friend.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Cohorts { friend auto access(Cohorts); };
template struct Access<Cohorts, &Adapter::cohorts_by_id_>;

constexpr std::int64_t kStartMs = 1736121600000LL;
constexpr std::uint64_t kBytesPerId = 64; // B_U = 64 * U, as the stated default does.

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

std::string order_id(std::size_t k) { return "order-" + std::to_string(k); }

// Opens follow a repeating pattern in quarter ticks: every difference below is
// exact in a double, and the trades do not all share one profit.
std::vector<pineforge::Bar> tape(std::size_t count) {
    std::vector<pineforge::Bar> bars(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double open = 100.0 + 0.25 * static_cast<double>((i * 37) % 101);
        bars[i] = {open, open + 1.0, open - 1.0, open, 1000.0,
                   kStartMs + static_cast<std::int64_t>(i) * 60000};
    }
    return bars;
}

// All maxima zero select the default manifest at each begin.
pf_scratch_tables_v1 bounded_descriptor() {
    pf_scratch_tables_v1 descriptor{};
    descriptor.struct_size = static_cast<std::uint32_t>(sizeof descriptor);
    descriptor.version = 1;
    descriptor.backend = PF_SCRATCH_TABLE_BOUNDED_MEMORY;
    return descriptor;
}

// An explicit manifest: every maximum but U and B_U is the default tier's.
pf_scratch_tables_v1 explicit_descriptor(std::uint64_t u, std::uint64_t b_u) {
    auto descriptor = bounded_descriptor();
    const auto tier = detail::default_scratch_manifest();
    descriptor.K_e = tier.K_e;
    descriptor.B_e = tier.B_e;
    descriptor.K_c = tier.K_c;
    descriptor.B_c = tier.B_c;
    descriptor.V = tier.V;
    descriptor.B_V = tier.B_V;
    descriptor.R = tier.R;
    descriptor.U = u;
    descriptor.B_U = b_u;
    return descriptor;
}

enum class Profile { disk, bounded_default, bounded_explicit };

// One market entry per bar under a distinct id, all held, then one close of the
// whole position. Entry k is placed at the close of bar k and fills at the open
// of bar k + 1; the close is placed at the close of bar `ids` and fills at the
// open of bar ids + 1.
class HoldAndCloseHost final : public pineforge::source::PineStrategyHost {
public:
    HoldAndCloseHost(std::size_t ids, Profile profile, std::uint64_t u = 0, std::uint64_t b_u = 0)
        : ids_(ids) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1e12;
        config.margin_long = config.margin_short = 0.0;
        config.pyramiding = 5000;
        configure_pine_strategy(config);
        set_syminfo_mintick(0.25);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        if (profile == Profile::disk) return;
        const auto descriptor = profile == Profile::bounded_default
            ? bounded_descriptor() : explicit_descriptor(u, b_u);
        if (set_scratch_tables_v1(&descriptor) != 0)
            setup_failure = "the bounded scratch tables were not selected";
    }

    void on_source_bar(const pineforge::Bar&) override {
        ++bars_seen;
        const auto bar = static_cast<std::size_t>(pine_bar_index());
        const auto na = std::numeric_limits<double>::quiet_NaN();
        if (bar == 0) {
            const auto& slab = scratch_tables_for_run();
            if (slab) {
                manifest = slab->manifest();
                has_manifest = true;
            }
        }
        if (bar < ids_) {
            if (bar + 1 == ids_) note_earlier_ids();
            strategy_entry(order_id(bar), true, na, na, 1.0);
            ++placed; // Not reached when the entry is refused.
        } else if (bar == ids_) {
            count_active_ids();
            strategy_close_all();
        }
    }

    std::size_t bars_seen = 0, placed = 0;
    std::string setup_failure;
    detail::ScratchManifest manifest{};
    bool has_manifest = false;
    // Before the last entry of the run is attempted: the ids already held.
    bool earlier_ids_present = false;
    std::uint64_t table_size_before_last = 0;
    // When every entry has filled and none is closed yet.
    std::size_t active_ids = 0;
    std::uint64_t logical_ids = 0;

private:
    void note_earlier_ids() {
        auto& table = adapter_.*access(Cohorts{});
        table_size_before_last = table.size();
        earlier_ids_present = true;
        for (std::size_t k = 0; k + 1 < ids_; ++k) {
            if (!table.contains(order_id(k))) {
                earlier_ids_present = false;
                break;
            }
        }
    }

    void count_active_ids() {
        auto& table = adapter_.*access(Cohorts{});
        active_ids = 0;
        for (const auto& row : table.resident_rows())
            if (!row.second.spillable()) ++active_ids;
        logical_ids = table.size();
    }

    std::size_t ids_;
};

std::string failure_text(const HoldAndCloseHost& host) {
    return std::string("code=") + pineforge::run_failure_code_of(host)
        + " args=" + pineforge::run_failure_args_of(host)
        + " text=\"" + host.last_error() + "\"";
}

void run_host(HoldAndCloseHost& host, const std::vector<pineforge::Bar>& bars) {
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
}

std::vector<pineforge::Trade> closed_trades(const HoldAndCloseHost& host) {
    std::vector<pineforge::Trade> rows;
    for (int i = 0; i < host.trade_count(); ++i) rows.push_back(host.get_trade(i));
    return rows;
}

// Expected values come from the tape alone: id k enters at the open of bar
// k + 1 and leaves at the open of bar ids + 1, one unit long, no costs.
void require_trades_match_tape(const std::vector<pineforge::Trade>& rows,
                               const std::vector<pineforge::Bar>& bars, std::size_t ids,
                               const std::string& label) {
    require(rows.size() == ids, label + ": " + std::to_string(rows.size())
            + " closed trades for " + std::to_string(ids) + " placed ids");
    std::map<std::string, const pineforge::Trade*> by_id;
    for (const auto& trade : rows) by_id.emplace(trade.entry_id, &trade);
    require(by_id.size() == ids, label + ": the closed trades do not carry distinct entry ids");
    const double exit_open = bars[ids + 1].open;
    double expected_profit = 0.0, actual_profit = 0.0;
    for (std::size_t k = 0; k < ids; ++k) {
        const auto found = by_id.find(order_id(k));
        require(found != by_id.end(), label + ": no closed trade for " + order_id(k));
        const auto& trade = *found->second;
        const double entry_open = bars[k + 1].open;
        const double expected = exit_open - entry_open;
        require(trade.is_long && trade.qty == 1.0
                && trade.entry_bar_index == static_cast<int>(k + 1)
                && trade.exit_bar_index == static_cast<int>(ids + 1)
                && trade.entry_price == entry_open && trade.exit_price == exit_open
                && trade.pnl == expected,
                label + ": " + order_id(k) + " differs from the tape (entry_bar="
                + std::to_string(trade.entry_bar_index) + " exit_bar="
                + std::to_string(trade.exit_bar_index) + " pnl=" + std::to_string(trade.pnl)
                + " expected=" + std::to_string(expected) + ")");
        expected_profit += expected;
        actual_profit += trade.pnl;
    }
    require(actual_profit == expected_profit, label + ": the total profit differs from the tape");
}

template<class T> void append_raw(std::string& out, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>, "raw bytes of a plain value");
    out.append(reinterpret_cast<const char*>(&value), sizeof value);
}

void append_text(std::string& out, const std::string& text) {
    append_raw(out, static_cast<std::uint64_t>(text.size()));
    out.append(text);
}

// Every field of a closed trade, as bytes: doubles compare bit for bit.
std::string trade_bytes(const pineforge::Trade& trade) {
    std::string out;
    append_raw(out, trade.entry_time);
    append_raw(out, trade.exit_time);
    append_raw(out, trade.entry_price);
    append_raw(out, trade.exit_price);
    append_raw(out, trade.qty);
    append_raw(out, trade.pnl);
    append_raw(out, trade.pnl_pct);
    append_raw(out, trade.is_long);
    append_raw(out, trade.entry_bar_index);
    append_raw(out, trade.exit_bar_index);
    append_text(out, trade.entry_id);
    append_text(out, trade.entry_comment);
    append_text(out, trade.exit_comment);
    append_text(out, trade.exit_id);
    append_raw(out, trade.exit_from_bracket);
    append_raw(out, trade.max_runup);
    append_raw(out, trade.max_drawdown);
    append_raw(out, trade.commission);
    append_raw(out, trade.entry_incarnation);
    append_raw(out, trade.open_at_end);
    append_raw(out, static_cast<int>(trade.close_cause));
    return out;
}

// The default manifest allows 65,536 ids; 4,097 are active at once, one more
// than the former fixed limit. Every id is placed and closed.
void default_manifest_holds_more_ids_than_the_former_limit() {
    constexpr std::size_t ids = 4097;
    const auto bars = tape(2 * ids + 2);
    HoldAndCloseHost host(ids, Profile::bounded_default);
    require(host.setup_failure.empty(), host.setup_failure);
    run_host(host, bars);
    require(host.last_error().empty(),
            "default manifest, " + std::to_string(ids) + " ids: the run stopped after "
            + std::to_string(host.bars_seen) + " bars with " + std::to_string(host.placed)
            + " ids placed: " + failure_text(host));
    require(host.has_manifest && host.manifest.U == 65536 && host.manifest.B_U == 4194304,
            "the run's manifest is not the stated default (U 65,536, B_U 4,194,304)");
    const auto defaults = detail::default_scratch_manifest(bars.size());
    require(host.manifest.U == defaults.U && host.manifest.B_U == defaults.B_U,
            "the run's manifest differs from default_scratch_manifest for this run");
    require(host.placed == ids, "not every id was placed");
    require(host.active_ids == ids && host.logical_ids == ids,
            "not every id was active at once before the close (active="
            + std::to_string(host.active_ids) + " logical=" + std::to_string(host.logical_ids) + ")");
    require_trades_match_tape(closed_trades(host), bars, ids, "default manifest");
}

// Under the former limit both backends place and close every id; their closed
// trades are the same bytes and both match the tape.
void under_the_former_limit_is_identical_on_both_backends() {
    constexpr std::size_t ids = 4095;
    const auto bars = tape(2 * ids + 2);
    HoldAndCloseHost disk(ids, Profile::disk);
    HoldAndCloseHost bounded(ids, Profile::bounded_default);
    require(disk.setup_failure.empty() && bounded.setup_failure.empty(), "backend selection failed");
    run_host(disk, bars);
    run_host(bounded, bars);
    require(disk.last_error().empty(), "disk backend: " + failure_text(disk));
    require(bounded.last_error().empty(), "bounded backend: " + failure_text(bounded));
    require(!disk.has_manifest && bounded.has_manifest,
            "only the bounded backend runs on a scratch slab");
    require(disk.active_ids == ids && bounded.active_ids == ids,
            "not every id was active at once before the close");
    const auto disk_trades = closed_trades(disk);
    const auto bounded_trades = closed_trades(bounded);
    require_trades_match_tape(disk_trades, bars, ids, "disk backend");
    require_trades_match_tape(bounded_trades, bars, ids, "bounded backend");
    require(disk_trades.size() == bounded_trades.size(), "the backends closed different trade counts");
    for (std::size_t i = 0; i < disk_trades.size(); ++i)
        require(trade_bytes(disk_trades[i]) == trade_bytes(bounded_trades[i]),
                "closed trade " + std::to_string(i) + " differs between the backends");
}

// Explicit maxima whose layout cannot be represented are refused before the
// first bar. The refusal comes from the layout arithmetic itself, so nothing is
// allocated, and no arena is attached.
void unrepresentable_maxima_are_refused_before_the_first_bar() {
    const auto largest = std::numeric_limits<std::uint64_t>::max();
    auto manifest = detail::default_scratch_manifest();
    manifest.U = largest; // 96 bytes per id times U overflows uint64_t on every host.
    manifest.B_U = 0;
    bool layout_refused = false;
    try {
        (void)detail::compute_layout(manifest);
    } catch (const detail::ScratchCapacityError&) {
        layout_refused = true;
    }
    require(layout_refused, "SETUP: compute_layout accepted the unrepresentable manifest");

    constexpr std::size_t ids = 8;
    const auto bars = tape(ids + 2);
    HoldAndCloseHost host(ids, Profile::bounded_explicit, manifest.U, manifest.B_U);
    require(host.setup_failure.empty(), host.setup_failure);
    run_host(host, bars);
    require(host.bars_seen == 0, "a bar was processed before the layout refusal");
    require(!host.last_error().empty(), "the unrepresentable manifest was not refused");
    require(std::string(pineforge::run_failure_code_of(host))
            == pineforge::run_failure_code_name(pineforge::RunFailureCode::scratch_capacity_exceeded),
            "the layout refusal has another code: " + failure_text(host));
    const auto expected_args = std::string("{\"resource\":\"")
        + detail::scratch_resource_name(detail::ScratchResource::layout) + "\"}";
    require(std::string(pineforge::run_failure_args_of(host)) == expected_args,
            "the layout refusal names another resource: " + failure_text(host));
    require(host.trade_count() == 0, "the refused run closed a trade");
}

// A host's explicit U is the limit, exactly: with U = 4,096 the 4,097th distinct
// active id is refused. The table's id-count ticket runs before its live-row
// ticket, so it is the id-count resource that names the refusal.
void explicit_maxima_are_honored_exactly() {
    constexpr std::uint64_t u = 4096;
    constexpr std::size_t ids = u + 1;
    const auto bars = tape(ids + 2);
    HoldAndCloseHost host(ids, Profile::bounded_explicit, u, kBytesPerId * u);
    require(host.setup_failure.empty(), host.setup_failure);
    run_host(host, bars);
    require(host.has_manifest && host.manifest.U == u && host.manifest.B_U == kBytesPerId * u,
            "the run did not keep the explicit manifest");
    require(host.earlier_ids_present && host.table_size_before_last == u,
            "the 4,096 earlier ids were not all placed before the refused one (table="
            + std::to_string(host.table_size_before_last) + ")");
    require(!host.last_error().empty(), "the 4,097th distinct id was admitted although U is 4,096");
    require(std::string(pineforge::run_failure_code_of(host))
            == pineforge::run_failure_code_name(pineforge::RunFailureCode::scratch_capacity_exceeded),
            "the refusal has another code: " + failure_text(host));
    const auto expected_args = std::string("{\"resource\":\"")
        + detail::scratch_resource_name(detail::ScratchResource::cohort_ids_total) + "\"}";
    require(std::string(pineforge::run_failure_args_of(host)) == expected_args,
            "the refusal names another resource: " + failure_text(host));
    const auto expected_text =
        detail::scratch_count_limit_message(detail::ScratchResource::cohort, u);
    require(host.last_error().find(expected_text) != std::string::npos,
            "the refusal text does not state the manifest's U: " + failure_text(host));
}
} // namespace

int main() {
    struct Row {
        const char* name;
        void (*run)();
    };
    // Every row runs even when an earlier one fails, and each reports on its own
    // line, so a single run shows which rows hold and which do not.
    const Row rows[] = {
        {"default_manifest_holds_more_ids_than_the_former_limit",
         default_manifest_holds_more_ids_than_the_former_limit},
        {"under_the_former_limit_is_identical_on_both_backends",
         under_the_former_limit_is_identical_on_both_backends},
        {"unrepresentable_maxima_are_refused_before_the_first_bar",
         unrepresentable_maxima_are_refused_before_the_first_bar},
        {"explicit_maxima_are_honored_exactly", explicit_maxima_are_honored_exactly},
    };
    bool every_row_passed = true;
    for (const auto& row : rows) {
        try {
            row.run();
            std::printf("%s OK\n", row.name);
        } catch (const std::exception& error) {
            every_row_passed = false;
            std::printf("%s FAIL: %s\n", row.name, error.what());
        } catch (...) {
            every_row_passed = false;
            std::printf("%s FAIL: an exception that is not a std::exception\n", row.name);
        }
        std::fflush(stdout);
    }
    if (!every_row_passed) return 1;
    std::puts("active order ids sized from run: OK");
    return 0;
}
