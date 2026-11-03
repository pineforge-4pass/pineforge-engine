// CR-725 / CR-726: mincontract supplies the quantity grid unless the handle
// has an explicit qty_step override. Uses only the base's source-host API;
// no private fixture, generated strategy, or new header member is required.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kRequest = 1.234567;

struct Group {
    const char* name;
    int matched = 0;
    int total = 0;

    void check(bool match, const char* row) {
        ++total;
        if (match) {
            ++matched;
        } else {
            std::printf("  FAIL %s: %s\n", name, row);
        }
    }

    int finish() const {
        std::printf("%s: %d / %d matched\n", name, matched, total);
        return total - matched;
    }
};

std::uint64_t bits(double value) {
    std::uint64_t result = 0;
    std::memcpy(&result, &value, sizeof result);
    return result;
}

pineforge::Bar flat_bar(int index, double price) {
    return {price, price, price, price, 1.0,
            1704067200000LL + 3600000LL * index};
}

class EntryProbe final : public pineforge::source::PineStrategyHost {
public:
    explicit EntryProbe(double request = kRequest, double capital = 100.0,
                        bool percent = false)
        : request_(request) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = capital;
        config.default_qty_type = static_cast<int>(
            percent ? pineforge::QtyType::PERCENT_OF_EQUITY : pineforge::QtyType::FIXED);
        config.default_qty_value = percent ? 100.0 : 1.0;
        config.commission_value = 0.0;
        config.slippage = 0;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        configure_pine_strategy(config);
        set_margin_call_enabled(false);
    }

    void on_source_bar(const pineforge::Bar&) override {
        if (pine_bar_index() == 0) {
            signal_equity = live_current_equity();
            signal_position = live_position_size();
            strategy_entry("L", true, kNaN, kNaN, request_);
        } else if (pine_bar_index() == 2) {
            strategy_close_all();
        }
    }

    // The same read-only getter generated syminfo.mincontract expressions use.
    double metadata(const std::string& key) const {
        return get_syminfo_metadata(key);
    }

    void run_entry(double signal_close = 1.0, double fill_open = 1.0) {
        const std::array<pineforge::Bar, 4> bars{{
            flat_bar(0, signal_close), flat_bar(1, fill_open),
            flat_bar(2, fill_open), flat_bar(3, fill_open),
        }};
        run(bars.data(), static_cast<int>(bars.size()));
    }

    double signal_equity = kNaN;
    double signal_position = kNaN;

private:
    double request_;
};

bool has_quantity(const EntryProbe& probe, double expected) {
    return probe.last_error().empty() && probe.trade_count() == 1
        && bits(probe.get_trade(0).qty) == bits(expected);
}

bool has_raw_quantity(const EntryProbe& probe) {
    return probe.last_error().empty() && probe.trade_count() == 1
        && bits(probe.get_trade(0).qty) == bits(kRequest);
}

int explicit_entries() {
    Group group{"(i) mincontract-only explicit entry"};
    // Documented floor rule (the quantity is floored to the declared step):
    // BTCUSDT 0.00001: literal 1.234567 -> 1.23456.
    // NYSE:F 1:       literal 1.234567 -> 1.
    // Limit: the engine floor adds a +1e-6-step epsilon; quantities near a
    // step boundary beyond the 29 trades and 59 rows of the private tests are
    // not covered.
    EntryProbe btc;
    btc.set_syminfo_metadata("mincontract", 0.00001);
    btc.run_entry();
    group.check(has_quantity(btc, 1.23456), "BTCUSDT");

    EntryProbe ford;
    ford.set_syminfo_metadata("mincontract", 1.0);
    ford.run_entry();
    group.check(has_quantity(ford, 1.0), "NYSE:F");
    return group.finish();
}

int override_precedence() {
    Group group{"(iii) explicit qty_step wins in either order"};
    const double overrides[] = {
        0.25, 0.0, -1.0, kNaN, std::numeric_limits<double>::infinity(),
    };
    for (double step : overrides) {
        for (bool explicit_first : {false, true}) {
            EntryProbe probe;
            if (explicit_first) probe.set_syminfo_metadata("qty_step", step);
            probe.set_syminfo_metadata("mincontract", 0.00001);
            if (!explicit_first) probe.set_syminfo_metadata("qty_step", step);
            probe.run_entry();
            group.check(step == 0.25 ? has_quantity(probe, 1.0) : has_raw_quantity(probe),
                        explicit_first ? "qty_step then mincontract" : "mincontract then qty_step");
        }
    }
    return group.finish();
}

int invalid_mincontract() {
    Group group{"(iv) invalid mincontract never sets or clears a grid"};
    const double invalid[] = {
        0.0, -1.0, kNaN, std::numeric_limits<double>::infinity(),
    };
    for (double value : invalid) {
        EntryProbe fresh;
        fresh.set_syminfo_metadata("mincontract", value);
        fresh.run_entry();
        group.check(has_raw_quantity(fresh), "invalid mincontract on a fresh handle");

        EntryProbe explicit_grid;
        explicit_grid.set_syminfo_metadata("qty_step", 0.25);
        explicit_grid.set_syminfo_metadata("mincontract", value);
        explicit_grid.run_entry();
        group.check(has_quantity(explicit_grid, 1.0), "invalid after explicit qty_step");

        EntryProbe retained;
        retained.set_syminfo_metadata("mincontract", 0.00001);
        retained.set_syminfo_metadata("mincontract", value);
        retained.run_entry();
        group.check(has_quantity(retained, 1.23456), "invalid after valid mincontract");
    }
    EntryProbe last_valid;
    last_valid.set_syminfo_metadata("mincontract", 0.00001);
    last_valid.set_syminfo_metadata("mincontract", 1.0);
    last_valid.run_entry();
    group.check(has_quantity(last_valid, 1.0), "last valid mincontract wins");
    return group.finish();
}

int no_key() {
    Group group{"(v) no key retains the base's zero-grid quantity bits"};
    EntryProbe absent;
    absent.run_entry();
    group.check(has_raw_quantity(absent), "fresh handle, neither key supplied");

    EntryProbe explicit_zero;
    explicit_zero.set_syminfo_metadata("qty_step", 0.0);
    explicit_zero.run_entry();
    group.check(has_raw_quantity(explicit_zero), "existing explicit zero-grid control");
    return group.finish();
}

int metadata_getter() {
    Group group{"(vi) mincontract getter returns the supplied value"};
    EntryProbe probe;
    group.check(std::isnan(probe.metadata("mincontract")), "absent metadata");
    probe.set_syminfo_metadata("qty_step", 0.25);
    const double values[] = {
        0.00001, 1.0, 0.0, -1.0, kNaN, std::numeric_limits<double>::infinity(),
    };
    for (double value : values) {
        probe.set_syminfo_metadata("mincontract", value);
        group.check(bits(probe.metadata("mincontract")) == bits(value), "raw metadata before run");
    }
    probe.run_entry();
    group.check(bits(probe.metadata("mincontract")) == bits(values[5]), "raw metadata after run");
    group.check(has_quantity(probe, 1.0), "getter writes do not replace the override");
    return group.finish();
}

int handle_lifetime() {
    Group group{"(vii) documented per-handle metadata lifetime"};
    // docs/pages/configuration.md: configuration applies until overwritten
    // or freed. lifecycle.md recommends a fresh handle for an A/B run.
    EntryProbe retained;
    retained.set_syminfo_metadata("mincontract", 0.00001);
    retained.run_entry();
    group.check(has_quantity(retained, 1.23456), "mincontract-only first run");
    retained.run_entry();
    group.check(has_quantity(retained, 1.23456), "second run retains the grid");
    group.check(bits(retained.metadata("mincontract")) == bits(0.00001),
                "second run retains the metadata");

    EntryProbe fresh;
    fresh.run_entry();
    group.check(has_raw_quantity(fresh), "fresh handle has no inherited grid");

    retained.set_syminfo_metadata("qty_step", 0.0);
    retained.run_entry();
    group.check(has_raw_quantity(retained), "explicit zero disables the retained grid");
    retained.set_syminfo_metadata("mincontract", 1.0);
    retained.run_entry();
    group.check(has_raw_quantity(retained), "explicit zero keeps precedence on later runs");
    return group.finish();
}

}  // namespace

int main() {
    int failures = 0;
    failures += explicit_entries();
    failures += override_precedence();
    failures += invalid_mincontract();
    failures += no_key();
    failures += metadata_getter();
    failures += handle_lifetime();
    std::printf("mincontract quantity-step regression: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
