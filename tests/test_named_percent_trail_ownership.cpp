// Ordinary named percentage trails carry an entry origin for reservations.
// That stamp does not make them PerEntryExit children. Exercise the actual
// native matcher after the reservation changes without an opening fill.
// These are reservation/lifecycle assertions, not new TradingView oracles.
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
constexpr double absent = std::numeric_limits<double>::quiet_NaN();
constexpr double opening = 10.0;
constexpr double drain_percent = 30.0;
constexpr std::int64_t start = 1'735'689'600'000LL;
constexpr std::int64_t interval = 900'000;

struct Row {
    const char* name;
    bool long_side;
    bool drain;
    bool coof;
    bool replace_quantity;
};
constexpr Row rows[] = {
    {"replace-long", true, false, false, true},
    {"replace-short", false, false, false, true},
    {"drain-offset-long", true, true, false, false},
    {"drain-offset-short", false, true, false, false},
    {"drain-coof-long", true, true, true, false},
    {"drain-coof-short", false, true, true, false},
    {"unchanged-long", true, false, false, false},
    {"unchanged-short", false, false, false, false},
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool near(double a, double b) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) < 1e-9;
}
double price(const Row& row, double value) {
    return row.long_side ? value : 200.0 - value;
}

class TrailHost final : public source::PineStrategyHost {
public:
    explicit TrailHost(const Row& row) : row_(row) {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config{};
        config.initial_capital = 1'000'000.0;
        config.pyramiding = 2;
        config.calc_on_order_fills = row.coof;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = opening;
        config.margin_long = config.margin_short = 100.0;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(1.0);
        set_syminfo_pointvalue(1.0);
        qty_step_ = 0.25;
        fixture_retain_all_events();
    }

    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        if (index == 0 && !entry_sent_) {
            entry_sent_ = true;
            strategy_entry("entry", row_.long_side, absent, absent, opening);
        }
        if (index == 1 && row_.drain && !add_sent_) {
            add_sent_ = true;
            strategy_entry("entry", row_.long_side, absent, absent, opening);
        }
        if (index < 2 || index > 4 || signed_position_size() == 0.0) return;
        if (row_.drain && !drain_sent_) {
            drain_sent_ = true;
            strategy_exit("other", "entry", price(row_, 110.0), absent,
                          absent, absent, absent, drain_percent);
        }
        // Capture the working predecessor before the changed call. Comparing
        // incarnation and rank catches a cancel/resubmit masquerading as a fix.
        if (index == 3 && !before_seen) {
            before = trails();
            before_priority = priorities();
            before_seen = true;
        }
        const bool changed = index >= 3;
        const double percent = row_.replace_quantity ? 75.0 : 50.0;
        const double quantity = row_.replace_quantity && changed ? opening / 4.0 : absent;
        const double offset = changed && !row_.coof
            && (row_.drain || row_.replace_quantity) ? 4.0 : 5.0;
        strategy_exit("trail", "entry", absent, absent, absent, offset,
                      price(row_, 120.0), percent, "", quantity);
        ++calls;
        if (index == 4) {
            final = trails();
            final_priority = priorities();
            observed = true;
            held_before_trail = std::abs(signed_position_size());
            for (const auto& lot : native_open_lots(absent))
                lots.emplace_back(lot.entry_incarnation, std::abs(lot.signed_units));
            std::sort(lots.begin(), lots.end());
        }
    }

    using Pending = source::PineExecutionAdapter::FixturePendingSnapshot;
    std::vector<Pending> trails() const {
        std::vector<Pending> result;
        for (const auto& pending : adapter_.fixture_pending_snapshots()) {
            if (pending.snapshot.source_id != "trail") continue;
            require(!pending.staged && pending.incarnation != 0,
                    "setup: trailing leg must be a native working request");
            result.push_back(pending);
        }
        require(result.size() == (row_.drain ? 2U : 1U),
                "setup: collect one trailing leg per selected opening");
        std::sort(result.begin(), result.end(), [](const Pending& a, const Pending& b) {
            return a.snapshot.per_entry_origin < b.snapshot.per_entry_origin;
        });
        return result;
    }

    std::vector<std::uint64_t> priorities() const {
        std::vector<std::uint64_t> result;
        for (const auto& working : native_working_requests()) {
            const auto& definition = *working.definition;
            if (definition.request.label == "trail")
                result.push_back(definition.priority != 0
                    ? definition.priority : definition.handle.incarnation);
        }
        return result;
    }

    const Row& row_;
    std::vector<Pending> before, final;
    std::vector<std::uint64_t> before_priority, final_priority;
    std::vector<std::pair<std::uint64_t, double>> lots;
    bool before_seen = false, observed = false;
    double held_before_trail = absent;
    int calls = 0;

private:
    bool entry_sent_ = false, add_sent_ = false, drain_sent_ = false;
};

std::vector<Bar> bars(const Row& row) {
    struct Prices { double open, high, low, close; };
    const std::array<Prices, 7> values{{
        {100, 101, 99, 100},
        {100, 101, 99, 100},   // first entry fills; optional second entry called
        {100, 101, 99, 100},   // second entry fills; trail/drain placed
        {100, 111, 99, 108},   // both drain children close the oldest lot FIFO
        {108, 109, 107, 108},  // observe changed reservation, no opening fill
        {108, 125, 107, 112},  // activate trail and cross its stop
        {112, 113, 111, 112},
    }};
    std::vector<Bar> result;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const auto& p = values[i];
        result.push_back({price(row, p.open),
                          price(row, row.long_side ? p.high : p.low),
                          price(row, row.long_side ? p.low : p.high),
                          price(row, p.close), 1000.0,
                          start + static_cast<std::int64_t>(i) * interval});
    }
    return result;
}

void check(const Row& row) {
    TrailHost host(row);
    const auto input = bars(row);
    host.run(input.data(), static_cast<int>(input.size()), "15", "15", false);
    require(host.last_error().empty(), "setup: native host reported an error");
    require(host.before_seen && host.observed && host.calls >= 3,
            "setup: replacement and observation must both execute");
    const int entry_count = row.drain ? 2 : 1;
    const double drained = row.drain ? entry_count * opening * drain_percent / 100.0 : 0.0;
    const double remaining = entry_count * opening - drained;
    require(near(host.held_before_trail, remaining),
            "setup: another exit must leave the declared partial exposure");
    require(host.before_priority.size() == static_cast<std::size_t>(entry_count)
            && host.before_priority == host.final_priority,
            "same-activation reissue must preserve every native queue priority");
    require(host.lots.size() == static_cast<std::size_t>(entry_count),
            "setup: each selected opening must still hold a native lot");
    std::vector<double> reservations;
    for (std::size_t i = 0; i < host.final.size(); ++i) {
        const auto& before = host.before[i];
        const auto& final = host.final[i];
        // Reissues preserve a partial reservation, clipping to remaining
        // capacity. The two 30% drains take 60% of the oldest lot FIFO.
        const double capacity = opening - (i == 0 ? drained : 0.0);
        const double reserved = row.replace_quantity ? opening / 4.0
            : std::min(opening / 2.0, capacity);
        reservations.push_back(reserved);
        require(host.lots[i].first == final.snapshot.per_entry_origin
                && near(host.lots[i].second, capacity),
                "setup: drain must reduce the oldest selected native lot FIFO");
        require(final.snapshot.per_entry_origin != 0
                && final.snapshot.bracket_origin.incarnation == final.snapshot.per_entry_origin,
                "setup: ordinary named trail must carry its reservation origin");
        require(final.snapshot.fixed_exit_reservation,
                "setup: named percentage trail must retain a fixed reservation");
        require(before.incarnation == final.incarnation,
                "same-activation reissue must preserve native trail identity");
        require(before.snapshot.chain_origin_sequence == final.snapshot.chain_origin_sequence
                && before.snapshot.source_sequence == final.snapshot.source_sequence,
                "same-activation reissue must preserve reservation sequence");
        require(near(final.snapshot.projection_remaining_qty, reserved),
                "setup: reservation must match explicit units or clipped prior capacity");
        if (i == 0 && (row.drain || row.replace_quantity))
            require(final.snapshot.per_entry_units > reserved,
                    "setup: witness must reach stale larger origin units before filling");
    }

    int entries = 0, drains = 0, trails = 0;
    double actual_drain = 0.0, actual_trail = 0.0;
    std::vector<std::pair<std::uint64_t, double>> trail_fills;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        const auto* fill = std::get_if<native_order::ExecutionAppliedEvent>(&*event.command);
        if (!fill) continue;
        if (std::abs(fill->opened_units) > 0.0) {
            ++entries;
            require(fill->request().label == "entry"
                    && near(std::abs(fill->opened_units), opening),
                    "setup: exactly the requested entry must open");
        }
        if (fill->request().label == "other" && fill->closed_units > 0.0) {
            ++drains;
            actual_drain += fill->closed_units;
            require(fill->interval_index() == 3,
                    "setup: drain children must execute before the trail observation");
        }
        if (fill->request().label == "trail" && fill->closed_units > 0.0) {
            ++trails;
            actual_trail += fill->closed_units;
            require(fill->interval_index() == 5,
                    "setup: trail must activate and fill on the crossing bar");
            trail_fills.emplace_back(fill->handle().incarnation, fill->closed_units);
        }
    }
    require(entries == entry_count && drains == (row.drain ? entry_count : 0)
            && near(actual_drain, drained), "setup: opening/drain fills did not match inputs");
    require(trails == entry_count, "setup: collect every native trailing fill");
    for (const auto& fill : trail_fills) {
        const auto observed = std::find_if(host.final.begin(), host.final.end(),
            [&](const TrailHost::Pending& pending) {
                return pending.incarnation == fill.first;
            });
        require(observed != host.final.end(),
                "every filling native trail must have an observed reservation");
        const auto index = static_cast<std::size_t>(observed - host.final.begin());
        std::printf("%s leg=%zu: reserved=%.17g origin_units=%.17g native_closed=%.17g\n",
                    row.name, index, reservations[index],
                    observed->snapshot.per_entry_units, fill.second);
        require(near(fill.second, reservations[index]),
                "ordinary named trail must fill its reservation, not stale origin units");
    }
    double total_reserved = 0.0;
    for (double units : reservations) total_reserved += units;
    require(near(actual_trail, total_reserved), "native trail fills must conserve reservations");
}
} // namespace

int main() {
    int failures = 0;
    for (const auto& row : rows) {
        try {
            check(row);
        } catch (const std::exception& error) {
            ++failures;
            std::fprintf(stderr, "%s: %s\n", row.name, error.what());
        }
    }
    return failures == 0 ? 0 : 1;
}
