// K5 must age Entry/Order rows in script-bar space.
// This TU uses the real Pine host and the existing retention reference switch.
// On d61e68b9e, the first split run retains a cancelled row beyond the previous
// script bar because K5 compares its input-slot projection with a script index.
// Red/green execution and the previous-epoch compile belong to the lab.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/source/pine_placement_retention.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;

void require(bool ok, const std::string& why) {
    if (!ok) throw std::runtime_error(why);
}

// The same private-member inspection used by test_adapter_live_state_equivalence;
// no production accessor or alternate retirement implementation is introduced.
template<class Tag, typename Tag::type Member>
struct PrivateAccess {
    friend typename Tag::type access(Tag) { return Member; }
};
struct PlacementTag {
    using type = source::PlacementTable source::PineExecutionAdapter::*;
    friend type access(PlacementTag);
};
template struct PrivateAccess<PlacementTag, &source::PineExecutionAdapter::placement_>;

struct SubmitTag {
    using type = std::optional<native_order::RequestHandle> (source::PineExecutionAdapter::*)(
        native_order::Request, source::PlacementSnapshot, bool, const source::SourceId&);
    friend type access(SubmitTag);
};
template struct PrivateAccess<SubmitTag, &source::PineExecutionAdapter::submit_or_replace>;

struct Fold {
    std::uint64_t h = 1469598103934665603ULL;
    void bytes(const void* data, std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    }
    void u(std::uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            const auto byte = static_cast<unsigned char>(value >> (i * 8));
            bytes(&byte, 1);
        }
    }
    void i(std::int64_t value) { u(static_cast<std::uint64_t>(value)); }
    void d(double value) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof bits);
        u(bits);
    }
    void s(const std::string& value) { u(value.size()); bytes(value.data(), value.size()); }
};

struct Path {
    const char* name;
    bool split;
    bool magnifier;
    bool sparse;
};

std::vector<Bar> make_bars(int script_bars, const Path& path) {
    std::vector<Bar> bars;
    const int ratio = path.split ? 15 : 1;
    for (int script = 0; script < script_bars; ++script) {
        // Vary the number of contributors and leave whole missing intervals:
        // script indices cannot be recovered by dividing an input slot by 15.
        const int bucket = script + (path.sparse ? script / 7 : 0);
        for (int minute = 0; minute < ratio; ++minute) {
            if (path.sparse && minute > 0 && minute < ratio - 1
                && (minute + script) % 3 == 0) continue;
            const double price = 100.0 + (script % 5) * 0.25;
            bars.push_back({price, price + 1.0, price - 1.0, price, 1000.0,
                            kStart + (bucket * ratio + minute) * 60000LL});
        }
    }
    return bars;
}

class RetentionReference {
public:
    explicit RetentionReference(bool retain) : prior_(source::detail::retain_retired_rows()) {
        source::detail::set_retain_retired_rows(retain);
    }
    ~RetentionReference() { source::detail::set_retain_retired_rows(prior_); }
private:
    bool prior_;
};

class K5Host final : public source::PineStrategyHost {
public:
    K5Host(bool reference, bool trade, bool coof)
        : reference_(reference), trade_(trade) {
        source::PineStrategyConfig config;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 2;
        config.calc_on_order_fills = coof;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }

    const source::PlacementTable& rows() const { return adapter_.*access(PlacementTag{}); }
    int callbacks = 0;
    int signals = 0;
    int split_coordinates = 0;
    std::size_t max_rows = 0;
    std::vector<std::uint64_t> transcript;

    void on_source_bar(const Bar&) override {
        const int script = pine_bar_index();
        const auto point = current_execution_point();
        require(point.has_value(), "fixture has no native decision point");
        require(point->decision.coordinate.interval_index == script,
                "fixture script index differs from the native coordinate");
        ++callbacks;
        transcript.push_back(observe());
        if (last_signal_ == script) return; // COOF may revisit the same script bar.
        require(script == last_signal_ + 1, "fixture skipped a script-bar callback");
        last_signal_ = script;
        ++signals;
        if (point->decision.coordinate.input_interval_index != script) ++split_coordinates;

        if (trade_) {
            // Real fills, with a reused entry id and raw transaction id. The
            // retaining run supplies the reference for pending rows and trades.
            if (script % 8 == 0) strategy_entry("L", true, kNa, kNa, 1.0);
            if (script % 8 == 2) strategy_close("L");
            if (script % 8 == 4) strategy_order("raw", true, 1.0);
            if (script % 8 == 6) strategy_order("raw", false, 1.0);
        } else {
            inspect_previous(script);
            // A working root must survive well beyond K5's two-bar window.
            if (script == 0) strategy_order("parked", true, 1.0, 1.0);
            strategy_entry("cancel-entry", true, 50.0, kNa, 1.0);
            // order() flushes preceding source entries before submitting itself.
            strategy_order("cancel-order", true, 1.0, 50.0);
            strategy_cancel("cancel-entry");
            strategy_cancel("cancel-order");
            int entries = 0, orders = 0;
            for (const auto& row : rows()) {
                const auto& value = row.second;
                if (value.birth.bar() != script) continue;
                if (value.source_id == "cancel-entry") ++entries;
                else if (value.source_id == "cancel-order") ++orders;
                else continue;
                require(value.projection_created_bar
                            == point->decision.coordinate.input_interval_index,
                        "input-slot pending projection was changed to a script index");
            }
            require(entries == 1 && orders == 1,
                    "fixture did not submit both cancelled placement families");
            max_rows = std::max(max_rows, rows().size());
            require(reference_ || rows().size() <= 5,
                    "K5 row population grew beyond two script bars plus the working root");
        }
        transcript.push_back(observe());
    }

    void hash_host_extension(BrokerStateHashSink& sink) const override {
        source::PineStrategyHost::hash_host_extension(sink);
        sink.s("cr150-l19f/v1");
        sink.i(reference_); sink.i(trade_); sink.i(last_signal_);
        // Counters/transcript only observe the run; they never drive commands.
    }

    std::uint64_t finish() {
        Fold f;
        f.u(observe());
        f.i(trade_count());
        for (int i = 0; i < trade_count(); ++i) {
            const Trade& t = get_trade(i);
            f.i(t.entry_time); f.i(t.exit_time); f.d(t.entry_price); f.d(t.exit_price);
            f.d(t.qty); f.d(t.pnl); f.d(t.pnl_pct); f.u(t.is_long);
            f.i(t.entry_bar_index); f.i(t.exit_bar_index); f.s(t.entry_id);
            f.s(t.entry_comment); f.s(t.exit_comment); f.s(t.exit_id);
            f.u(t.exit_from_bracket); f.d(t.max_runup); f.d(t.max_drawdown);
            f.d(t.commission); f.u(t.entry_incarnation); f.u(t.open_at_end);
            f.i(static_cast<std::int64_t>(t.close_cause));
        }
        f.u(equity_curve_.size());
        for (const auto& point : equity_curve_) {
            f.i(point.time_ms); f.d(point.equity); f.d(point.open_profit);
        }
        return f.h;
    }

private:
    void inspect_previous(int script) const {
        bool entry = false, order = false, parked = false;
        for (const auto& row : rows()) {
            const auto& value = row.second;
            if (value.source_id == "parked") { parked = true; continue; }
            const bool is_entry = value.source_id == "cancel-entry";
            const bool is_order = value.source_id == "cancel-order";
            require(is_entry || is_order, "fixture retained an unexpected placement");
            require(reference_ || value.birth.bar() >= script - 1,
                    "K5 retained a cancelled placement older than the previous script bar: birth="
                        + std::to_string(value.birth.bar()) + " input="
                        + std::to_string(value.projection_created_bar) + " script="
                        + std::to_string(script));
            if (value.birth.bar() == script - 1) {
                entry = entry || is_entry;
                order = order || is_order;
            }
        }
        require(script == 0 || (entry && order && parked),
                "K5 lost a previous-script command or an older working root");
    }

    std::uint64_t observe() {
        Fold f;
        f.i(pine_bar_index()); f.d(live_position_size());
        f.d(position_avg_price()); f.d(live_current_equity()); f.i(trade_count());
        auto handle = static_cast<pf_strategy_t>(static_cast<BacktestEngine*>(this));
        const int pending = strategy_pending_orders_len(handle);
        require(pending >= 0, "pending projection failed");
        f.i(pending);
        for (int i = 0; i < pending; ++i) {
            pf_pending_order_v1_t row;
            std::memset(&row, 0, sizeof row);
            require(strategy_pending_order_get(handle, i, &row, sizeof row) == 0,
                    "pending row read failed");
            f.bytes(&row, sizeof row);
        }
        return f.h;
    }

    bool reference_;
    bool trade_;
    int last_signal_ = -1;
};

// A delayed/recreated command preserves its immutable birth but is submitted
// on a later bar. Exercise that production boundary directly, with a real
// native request and cancellation; no model of the retirement predicate.
class LatePlacementHost final : public source::PineStrategyHost {
public:
    LatePlacementHost() {
        source::PineStrategyConfig config;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }

    int callbacks = 0;
    const source::PlacementTable& rows() const { return adapter_.*access(PlacementTag{}); }

    void on_source_bar(const Bar&) override {
        ++callbacks;
        const int script = pine_bar_index();
        if (script == 3) {
            const auto point = current_execution_point();
            require(point.has_value(), "late placement has no callback point");
            input_ = point->decision.coordinate.input_interval_index;
            for (std::size_t i = 0; i < handles_.size(); ++i) {
                const std::string id = i == 0 ? "late-entry" : "late-order";
                native_order::Request request;
                request.label = id;
                request.intent = native_order::Transact{1.0};
                request.trigger = native_order::Limit{50.0};
                source::PlacementSnapshot snapshot;
                snapshot.family = i == 0 ? source::PineOrderFamily::Entry
                                         : source::PineOrderFamily::Order;
                snapshot.source_id = id;
                snapshot.requested_qty = 1.0;
                snapshot.exit_levels.limit = 50.0;
                snapshot.birth = OrderBirth::chart_evaluation(0, kStart);
                snapshot.sizing.equity = 1000000.0;
                snapshot.sizing.price = 100.0;
                snapshot.sizing.mark = 100.0;
                snapshot.sizing.fx = 1.0;
                snapshot.sizing.frozen_units = 1.0;
                const auto accepted = (adapter_.*access(SubmitTag{}))(
                    std::move(request), std::move(snapshot), true, id);
                require(accepted.has_value(), "late placement was not accepted");
                handles_[i] = accepted->incarnation;
                strategy_cancel(id);
                const auto& row = rows().at(handles_[i]);
                require(row.birth.bar() == 0 && row.projection_created_bar == input_,
                        "late submission changed birth or failed to stamp the input projection");
            }
        }
        if (script == 4) {
            for (const auto handle : handles_) {
                require(rows().contains(handle),
                        "K5 aged a late placement by its older command birth");
                require(rows().at(handle).birth.bar() == 0
                            && rows().at(handle).projection_created_bar == input_,
                        "late placement provenance changed while retained");
            }
        }
        if (script == 5) {
            for (const auto handle : handles_)
                require(!rows().contains(handle), "late placement outlived its K5 window");
        }
    }

    void hash_host_extension(BrokerStateHashSink& sink) const override {
        source::PineStrategyHost::hash_host_extension(sink);
        sink.s("cr150-l19f/late-placement/v1");
        sink.i(callbacks); sink.i(input_);
        for (const auto handle : handles_) sink.u(handle);
    }

private:
    std::array<std::uint64_t, 2> handles_{};
    int input_ = -1;
};

void run_late_case(const Path& path) {
    const RetentionReference retention(false);
    const auto bars = make_bars(8, path);
    LatePlacementHost host;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", path.split ? "15" : "1",
             path.magnifier, 4, MagnifierDistribution::ENDPOINTS);
    require(host.last_error().empty(), std::string(path.name) + ": " + host.last_error());
    require(host.callbacks == 8 && host.rows().high_water() == 2
                && host.rows().erased_rows() == 2 && host.rows().size() == 0,
            "late placement fixture did not complete both retirement boundaries");
    require(host.trade_count() == 0 && host.native_working_requests().empty(),
            "late placement fixture filled a cancelled request");
}

struct Outcome {
    std::vector<std::uint64_t> transcript;
    std::uint64_t final = 0;
    std::size_t rows = 0, max_rows = 0;
    std::uint64_t placed = 0, erased = 0;
    int trades = 0;
};

Outcome run_case(const Path& path, int scripts, bool reference, bool trade, bool coof = false) {
    const RetentionReference retention(reference);
    const auto bars = make_bars(scripts, path);
    K5Host host(reference, trade, coof);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", path.split ? "15" : "1",
             path.magnifier, 4, MagnifierDistribution::ENDPOINTS);
    require(host.last_error().empty(), std::string(path.name) + ": " + host.last_error());
    require(host.signals == scripts && host.callbacks >= scripts,
            "fixture did not cover its script-bar population");
    require(!path.split || host.split_coordinates > scripts / 2,
            "split fixture never separated input and script indices");
    if (trade) require(host.trade_count() > 0, "differential fixture produced no closed trades");
    else require(host.trade_count() == 0 && host.native_working_requests().size() == 1,
                 "cancelled requests filled or the old working root was lost");
    Outcome out;
    out.final = host.finish();
    out.transcript = std::move(host.transcript);
    out.rows = host.rows().size(); out.max_rows = host.max_rows;
    out.placed = host.rows().high_water(); out.erased = host.rows().erased_rows();
    out.trades = host.trade_count();
    return out;
}

void compare(const Outcome& actual, const Outcome& reference) {
    require(actual.transcript == reference.transcript && actual.final == reference.final
                && actual.trades == reference.trades,
            "retirement changed pending orders, position, equity or closed trades");
}
} // namespace

int main() {
    try {
        for (const Path& path : {
                 Path{"aligned", false, false, false},
                 Path{"aggregate", true, false, false},
                 Path{"magnifier", true, true, false},
                 Path{"sparse-aggregate", true, false, true},
                 Path{"sparse-magnifier", true, true, true}}) {
            run_late_case(path);
            const auto small = run_case(path, 24, false, false);
            const auto large = run_case(path, 96, false, false);
            const auto retained = run_case(path, 96, true, false);
            require(small.max_rows == large.max_rows && large.rows <= 5 && large.erased > 0,
                    "K5 retained-row population is not flat at N/4N");
            require(retained.rows == retained.placed && retained.erased == 0
                        && retained.rows == 193,
                    "retention control failed to retain all submitted rows");
            compare(large, retained);
            for (const bool coof : {false, true}) {
                compare(run_case(path, 32, false, true, coof),
                        run_case(path, 32, true, true, coof));
            }
        }
        std::puts("K5 script-bar retirement: late placements, boundaries, N/4N and differentials passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "K5 script-bar retirement: %s\n", error.what());
        return 1;
    }
}
