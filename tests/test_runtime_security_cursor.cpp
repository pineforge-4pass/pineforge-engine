// N6 cursor source rows. WRITTEN / UNRUN; classifications follow CORE18:37/L618:39.
// TV CONTRACT: scalar lookaheadON prefixes/complete flag; OFF completed bars;
// lower-timeframe arrays contain completed elements even with lookaheadON.
// IMPLEMENTATION DETAIL: chart_open is first-observed/crossing input time;
// one TLS (host,cursor) slot, peer hiding/root fallback and unwind restoration.
// METADATA ONLY: gaps_on has no tested NA/result-envelope rule in these rows.
// Expected bars below are hand-computed literals, never product aggregation.
#include <pineforge/bar.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/runtime_context.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
using source::PineRuntimeSecurityCursor;
using source::PineRuntimeSecurityScope;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

void number(double actual, double expected, const std::string& field) {
    if (std::isnan(expected)) {
        require(std::isnan(actual), field + ": expected NA");
    } else {
        require(std::isfinite(actual) && std::abs(actual - expected) <= 1e-12,
                field + ": actual=" + std::to_string(actual)
                    + " expected=" + std::to_string(expected));
    }
}

void equal_bar(const Bar& actual, const Bar& expected, const std::string& label) {
    number(actual.open, expected.open, label + ".open");
    number(actual.high, expected.high, label + ".high");
    number(actual.low, expected.low, label + ".low");
    number(actual.close, expected.close, label + ".close");
    number(actual.volume, expected.volume, label + ".volume");
    require(actual.timestamp == expected.timestamp, label + ".timestamp");
}

class Host final : public source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {}

    // Exercise the real public begin/run path, without assigning host fields.
    void prepare(const std::string& input, const std::string& chart,
                 std::int64_t input_ms = 60000) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        const std::array<Bar, 2> seed{{
            {100, 102, 97, 99, 7, 0},
            {99, 104, 98, 103, 11, input_ms},
        }};
        run(seed.data(), static_cast<int>(seed.size()), input, chart);
        require(last_error().empty(), "host prepare: " + last_error());
        require(main_period() == chart, "host chart clock was not installed");
    }
};

// Three unequal, direction-changing requested windows. All inputs matter:
// raw 3-minute outputs are (10,20,6,8,V15), (30,40,20,22,V41),
// and (5,12,1,10,V71), at 0, 180000, and 360000.
const std::array<Bar, 9> inputs{{
    {10, 18, 8, 14, 3, 0},
    {14, 20, 11, 12, 5, 60000},
    {12, 16, 6, 8, 7, 120000},
    {30, 36, 26, 34, 11, 180000},
    {34, 40, 25, 28, 13, 240000},
    {28, 32, 20, 22, 17, 300000},
    {5, 9, 2, 4, 19, 360000},
    {4, 7, 1, 6, 23, 420000},
    {6, 12, 3, 10, 29, 480000},
}};

const std::array<Bar, 9> raw_prefixes{{
    {10, 18, 8, 14, 3, 0},
    {10, 20, 8, 12, 8, 0},
    {10, 20, 6, 8, 15, 0},
    {30, 36, 26, 34, 11, 180000},
    {30, 40, 25, 28, 24, 180000},
    {30, 40, 20, 22, 41, 180000},
    {5, 9, 2, 4, 19, 360000},
    {5, 9, 1, 6, 42, 360000},
    {5, 12, 1, 10, 71, 360000},
}};

struct Row {
    Bar bar;
    bool complete;
    bool slot_new;
    std::int64_t requested_index;
    int sub_bar_index;
    std::int64_t chart_open;
    bool ta_installed;
    long long ta_index;
    long long ta_origin;
};

struct Capture {
    std::vector<Row> rows;
    std::vector<std::vector<Row>> arrays;
    std::vector<std::int64_t> starts;
    std::vector<std::size_t> begin_positions;
    double latest = std::numeric_limits<double>::quiet_NaN();

    void feed(PineRuntimeSecurityCursor& cursor, const Bar& bar,
              std::uint64_t input_index) {
        const ta::BarContext before = ta::bar_context();
        cursor.feed({bar, input_index, input_index + 1},
            [&](const Bar& delivered, bool complete) {
                require(!arrays.empty(), "delivery preceded begin_chart");
                const auto context = ta::bar_context();
                Row row{delivered, complete, cursor.slot_new,
                        cursor.requested_index, cursor.sub_bar_index,
                        cursor.chart_open, context.installed,
                        context.bar_index, context.origin};
                rows.push_back(row);
                arrays.back().push_back(row);
                latest = delivered.close;
            },
            [&] {
                require(cursor.chart_open == bar.timestamp, "implementation detail: begin_chart observed input timestamp");
                require(cursor.sub_bar_index == 0, "begin_chart resets array index");
                starts.push_back(cursor.chart_open);
                begin_positions.push_back(rows.size());
                arrays.emplace_back();
            });
        const auto after = ta::bar_context();
        require(after.installed == before.installed
                    && after.bar_index == before.bar_index
                    && after.origin == before.origin,
                "feed did not restore caller TA context");
        require(cursor.prior_input == bar.timestamp, "feed prior-input clock");
    }
};

void row(const Row& actual, const Bar& expected, bool complete, bool slot_new,
         std::int64_t index, int sub_index, std::int64_t chart_open,
         const std::string& label) {
    equal_bar(actual.bar, expected, label);
    require(actual.complete == complete, label + ".complete");
    require(actual.slot_new == slot_new, label + ".slot_new");
    require(actual.requested_index == index, label + ".requested_index");
    require(actual.sub_bar_index == sub_index, label + ".sub_bar_index");
    require(actual.chart_open == chart_open, label + ".chart_open [implementation detail]");
    require(actual.ta_installed, label + ".TA installed");
    require(actual.ta_index == index, label + ".TA requested index");
    require(actual.ta_origin == 0, label + ".TA origin");
}

void pristine(const PineRuntimeSecurityCursor& cursor,
              const std::string& requested, const std::string& chart,
              bool lookahead, bool gaps, bool lower, bool ha) {
    require(cursor.timeframe == requested, "factory requested timeframe");
    require(cursor.chart_timeframe == chart, "factory enclosing timeframe");
    require(cursor.timezone == "UTC" && cursor.session == "24x7",
            "factory symbol clock");
    // gaps_on is checked only as stored metadata, never as an NA policy.
    require(cursor.lookahead_on == lookahead && cursor.gaps_on == gaps
                && cursor.lower_array == lower && cursor.heikinashi == ha,
            "factory request flags");
    require(!cursor.ha_seeded && cursor.ha_open == 0 && cursor.ha_close == 0,
            "factory HA state");
    require(cursor.requested_index == -1 && cursor.sub_bar_index == 0
                && cursor.slot_new, "factory indices");
    const auto unset = std::numeric_limits<std::int64_t>::min();
    require(cursor.slot_open == unset && cursor.chart_open == unset
                && cursor.prior_input == unset, "factory clock sentinels");
}

template<class F>
void factory_error(F&& body, RunFailureCode code, const std::string& args,
                   const std::string& text) {
    bool caught = false;
    try {
        body();
    } catch (const std::runtime_error& error) {
        caught = true;
        const auto* info = dynamic_cast<const RunFailureInfo*>(&error);
        require(info != nullptr, "factory error lacks structured failure");
        const auto failure = classify_run_failure(error);
        require(failure.code == code && info->run_failure_code() == code,
                "factory failure code");
        require(failure.args && *failure.args == args, "factory failure arguments");
        require(std::string(error.what()) == text, "factory failure text");
    }
    require(caught, "factory unexpectedly accepted invalid request");
}

void factory_validation() {
    Host unknown;
    factory_error([&] { unknown.make_runtime_security_cursor("3", false, false, false); },
        RunFailureCode::run_options_rejected, "{\"option\":\"input_timeframe\"}",
        "request.security cannot infer input timeframe; pass input_tf explicitly");

    Host host;
    host.prepare("1", "6");
    for (bool lower : {false, true}) {
        const std::string api = lower ? "request.security_lower_tf" : "request.security";
        factory_error([&] {
            host.make_runtime_security_cursor("bogus", false, false, lower);
        }, RunFailureCode::request_timeframe_invalid, "{\"api\":\"" + api + "\"}",
           api + ": invalid timeframe literal 'bogus'");
    }
    for (const std::string selected : {"6", "12", "M"}) {
        factory_error([&] {
            host.make_runtime_security_cursor(selected, false, false, true);
        }, RunFailureCode::request_timeframe_unsupported,
           "{\"reason\":\"lower_tf_not_finer_than_script\",\"script_tf\":\"6\"}",
           "request.security_lower_tf: requested timeframe must be finer than the chart timeframe");
    }
    factory_error([&] { host.make_runtime_security_cursor("4", false, false, true); },
        RunFailureCode::request_timeframe_unsupported,
        "{\"reason\":\"lower_tf_not_divisor_of_script\",\"script_tf\":\"6\"}",
        "request.security_lower_tf: requested timeframe must evenly divide the chart timeframe");

    Host calendar;
    calendar.prepare("1", "M");
    factory_error([&] { calendar.make_runtime_security_cursor("1", false, false, true); },
        RunFailureCode::request_timeframe_unsupported,
        "{\"reason\":\"lower_tf_script_unknown\"}",
        "request.security_lower_tf: script timeframe is unknown");

    Host coarse;
    coarse.prepare("6", "30", 360000);
    factory_error([&] { coarse.make_runtime_security_cursor("5", false, false, true); },
        RunFailureCode::request_timeframe_unsupported,
        "{\"input_tf\":\"6\",\"reason\":\"lower_tf_not_divisor_of_input\"}",
        "request.security_lower_tf: requested timeframe is not an integer divisor of input");

    Host same;
    same.prepare("3", "3", 180000);
    factory_error([&] { same.make_runtime_security_cursor("1", false, false, false); },
        RunFailureCode::request_timeframe_unsupported,
        "{\"input_tf\":\"3\",\"reason\":\"finer_than_input\"}",
        "request.security: requested timeframe is finer than input; supply finer bars");

    auto valid = host.make_runtime_security_cursor("3", true, true, false, true);
    pristine(valid, "3", "6", true, true, false, true);
    require(!valid.no_loaded_bars && !valid.subdivision, "valid aggregate route");
    auto absent_calendar = calendar.make_runtime_security_cursor("30S", false, false, false);
    pristine(absent_calendar, "30S", "M", false, false, false, false);
    require(absent_calendar.no_loaded_bars && !absent_calendar.subdivision,
            "calendar chart finer-than-input empty route");
    host.set_syminfo_timezone("Asia/Taipei");
    host.set_syminfo_session("0900-1700");
    const auto symbol_clock = host.make_runtime_security_cursor("3", false, false, false);
    require(symbol_clock.timezone == "Asia/Taipei" && symbol_clock.session == "0900-1700",
            "factory copies configured symbol clock");
}

struct Unwind {};

// IMPLEMENTATION DETAIL: one TLS (host,cursor) slot, including peer hiding.
// These identity/inheritance/unwind expectations have no TV surface.
void inherited_scopes_and_identity() {
    Host host, peer;
    host.prepare("1", "12");
    peer.prepare("1", "9");
    auto parent = host.make_runtime_security_cursor("6", false, false, false);
    require(PineRuntimeSecurityScope::current(host) == nullptr, "initial scope");
    require(host.runtime_security_timeframe("") == "12", "root inheritance");
    {
        PineRuntimeSecurityScope outer(host, parent);
        require(PineRuntimeSecurityScope::current(host) == &parent, "parent identity");
        require(PineRuntimeSecurityScope::current(peer) == nullptr, "owner isolation");
        auto child = host.make_runtime_security_cursor("", true, false, false);
        pristine(child, "6", "6", true, false, false, false);
        auto finer = host.make_runtime_security_cursor("3", false, false, true);
        pristine(finer, "3", "6", false, false, true, false);
        try {
            PineRuntimeSecurityScope inner(host, finer);
            require(PineRuntimeSecurityScope::current(host) == &finer, "child identity");
            require(host.runtime_security_timeframe("") == "3", "child inheritance");
            require(host.runtime_security_timeframe("2") == "2", "explicit override");
            auto grandchild = host.make_runtime_security_cursor("", false, false, false);
            pristine(grandchild, "3", "3", false, false, false, false);
            throw Unwind{};
        } catch (const Unwind&) {}
        require(PineRuntimeSecurityScope::current(host) == &parent, "unwind identity");
        require(host.runtime_security_timeframe("") == "6", "unwind inheritance");
        auto other = peer.make_runtime_security_cursor("", false, false, false);
        pristine(other, "9", "9", false, false, false, false);
        {
            PineRuntimeSecurityScope peer_scope(peer, other);
            require(PineRuntimeSecurityScope::current(peer) == &other, "peer identity");
            require(PineRuntimeSecurityScope::current(host) == nullptr, "peer hides owner");
            require(host.runtime_security_timeframe("") == "12", "peer root fallback");
        }
        require(PineRuntimeSecurityScope::current(host) == &parent, "peer unwind");
    }
    require(PineRuntimeSecurityScope::current(host) == nullptr
                && PineRuntimeSecurityScope::current(peer) == nullptr, "scope exit");
    host.prepare("1", "3");
    auto reset = host.make_runtime_security_cursor("", false, false, false);
    pristine(reset, "3", "3", false, false, false, false);
}

// TV CONTRACT: scalar partial/completed delivery and complete flags.
// chart_open assertions below remain IMPLEMENTATION DETAIL characterization.
void chart_boundaries_and_publication() {
    Host host;
    host.prepare("1", "6");
    for (bool lookahead : {false, true}) {
        // gaps_on is factory metadata here; the cursor has no result/NA envelope.
        auto cursor = host.make_runtime_security_cursor("3", lookahead, true, false);
        Capture out;
        ta::BarContextScope chart_context(73, 19);
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            out.feed(cursor, inputs[i], i);
            const auto count = lookahead ? i + 1 : (i + 1) / 3;
            require(out.rows.size() == count, "publication callback count per input");
            if (count == 0) {
                number(out.latest, std::numeric_limits<double>::quiet_NaN(),
                       "no completed publication");
                require(cursor.requested_index == -1, "no requested slot before delivery");
                require(cursor.slot_open == std::numeric_limits<std::int64_t>::min(),
                        "no slot timestamp before delivery");
            } else {
                const std::size_t delivered_input = lookahead ? i : count * 3 - 1;
                number(out.latest, raw_prefixes[delivered_input].close, "latest publication");
                require(cursor.requested_index
                            == static_cast<std::int64_t>(delivered_input / 3),
                        "cursor requested index after input");
            }
            require(cursor.sub_bar_index == 0, "scalar request has no lower-array count");
        }
        require(out.starts == std::vector<std::int64_t>({0, 360000}),
                "implementation detail: chart begins at observed/crossing input times");
        require(out.begin_positions == (lookahead ? std::vector<std::size_t>{0, 6}
                                                   : std::vector<std::size_t>{0, 2}),
                "begin_chart precedes new chart deliveries");
        for (std::size_t j = 0; j < out.rows.size(); ++j) {
            const std::size_t i = lookahead ? j : j * 3 + 2;
            row(out.rows[j], raw_prefixes[i], i % 3 == 2,
                !lookahead || i % 3 == 0, static_cast<std::int64_t>(i / 3), 0,
                i < 6 ? 0 : 360000, "scalar row " + std::to_string(j));
        }
        require(cursor.slot_open == 360000 && cursor.chart_open == 360000,
                "terminal requested and chart clocks");
    }
}

void lower_arrays_every_element() {
    Host host;
    host.prepare("1", "6");
    // TV CONTRACT: lower arrays contain completed bars even with lookaheadON.
    auto cursor = host.make_runtime_security_cursor("2", true, false, true);
    Capture out;
    const std::array<double, 9> latest{{
        std::numeric_limits<double>::quiet_NaN(), 12, 12, 34, 34, 22, 22, 6, 6,
    }};
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        out.feed(cursor, inputs[i], i);
        require(out.rows.size() == (i + 1) / 2, "lower aggregate callback count");
        const int expected_count = i < 6 ? static_cast<int>((i + 1) / 2)
                                         : static_cast<int>((i - 5) / 2);
        require(cursor.sub_bar_index == expected_count, "lower aggregate count/reset");
        number(out.latest, latest[i], "lower latest completed value");
        require(cursor.requested_index == static_cast<std::int64_t>((i + 1) / 2) - 1,
                "lower requested index after every input");
    }
    const std::array<Bar, 4> expected{{
        {10, 20, 8, 12, 8, 0},
        {12, 36, 6, 34, 18, 120000},
        {34, 40, 20, 22, 30, 240000},
        {5, 9, 1, 6, 42, 360000},
    }};
    require(out.starts == std::vector<std::int64_t>({0, 360000}), "lower chart starts");
    require(out.begin_positions == std::vector<std::size_t>({0, 3}), "lower begin order");
    require(out.arrays.size() == 2 && out.arrays[0].size() == 3
                && out.arrays[1].size() == 1, "every chart array count");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto chart = i < 3 ? 0u : 1u;
        const auto offset = i < 3 ? i : 0u;
        row(out.arrays[chart][offset], expected[i], true, true,
            static_cast<std::int64_t>(i), static_cast<int>(offset),
            chart == 0 ? 0 : 360000, "lower array element " + std::to_string(i));
    }
    // The ninth input leaves a partial requested bar; no invented fifth element.
    require(cursor.requested_index == 3 && cursor.slot_open == 360000,
            "lower incomplete tail did not advance published slot");
}

// Completed lower elements are contract; synthetic path/order values are characterization.
void subdivision_every_element_and_order() {
    Host host;
    host.prepare("3", "6", 180000);
    auto cursor = host.make_runtime_security_cursor("1", false, false, true);
    require(cursor.subdivision && cursor.subdivision->ratio == 3
                && cursor.subdivision->seconds == 60 && !cursor.no_loaded_bars,
            "factory subdivision route");
    // Distances select opposite first turns: 10 -> 8 -> 18 -> 14,
    // then 30 -> 34 -> 20 -> 22, then 5 -> 1 -> 15 -> 12.
    const std::array<Bar, 3> coarse{{
        {10, 18, 8, 14, 15, 0},
        {30, 34, 20, 22, 24, 180000},
        {5, 15, 1, 12, 33, 360000},
    }};
    const std::array<Bar, 9> expected{{
        {10, 10, 8, 8, 5, 0},
        {8, 18, 8, 18, 5, 60000},
        {18, 18, 14, 14, 5, 120000},
        {30, 34, 30, 34, 8, 180000},
        {34, 34, 20, 20, 8, 240000},
        {20, 22, 20, 22, 8, 300000},
        {5, 5, 1, 1, 11, 360000},
        {1, 15, 1, 15, 11, 420000},
        {15, 15, 12, 12, 11, 480000},
    }};
    Capture out;
    for (std::size_t i = 0; i < coarse.size(); ++i) {
        out.feed(cursor, coarse[i], i);
        require(out.rows.size() == (i + 1) * 3, "subdivision count per input");
        require(cursor.sub_bar_index == (i == 1 ? 6 : 3), "subdivision chart count");
    }
    require(out.starts == std::vector<std::int64_t>({0, 360000}), "subdivision starts");
    require(out.begin_positions == std::vector<std::size_t>({0, 6}), "subdivision begin order");
    require(out.arrays.size() == 2 && out.arrays[0].size() == 6
                && out.arrays[1].size() == 3, "subdivision array sizes");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto chart = i < 6 ? 0u : 1u;
        const auto offset = i < 6 ? i : i - 6;
        row(out.arrays[chart][offset], expected[i], true, true,
            static_cast<std::int64_t>(i), static_cast<int>(offset),
            chart == 0 ? 0 : 360000, "subdivision element " + std::to_string(i));
    }
    require(cursor.requested_index == 8, "subdivision terminal requested index");
}

// Empty-route/NA state is characterization, not a gaps_on result-envelope rule.
void no_loaded_bars_has_no_publication() {
    Host host;
    host.prepare("3", "6", 180000);
    for (bool lookahead : {false, true}) {
        auto cursor = host.make_runtime_security_cursor("1", lookahead, true, false);
        require(cursor.no_loaded_bars && !cursor.subdivision, "factory empty route");
        Capture out;
        for (std::size_t i = 0; i < inputs.size(); i += 3) {
            out.feed(cursor, inputs[i], i / 3);
            require(out.rows.empty() && cursor.requested_index == -1
                        && cursor.sub_bar_index == 0, "empty route must not publish");
            number(out.latest, std::numeric_limits<double>::quiet_NaN(), "empty route NA");
        }
        require(out.starts == std::vector<std::int64_t>({0, 360000}),
                "empty route still advances chart clock");
        require(out.begin_positions == std::vector<std::size_t>({0, 0}),
                "empty route begin positions");
        require(out.arrays.size() == 2 && out.arrays[0].empty() && out.arrays[1].empty(),
                "empty route arrays");
        require(cursor.slot_open == std::numeric_limits<std::int64_t>::min()
                    && !cursor.ha_seeded, "empty route preserves unpublished state");
    }
}

// Scalar delivery/complete flags are contract; chart_open remains implementation detail.
void heikin_ashi_complete_and_partial() {
    Host host;
    host.prepare("1", "6");
    // HA close = (O+H+L+C)/4. Before first completion open=(O+C)/2.
    // Completed seeds: (9,11), (10,28), (19,7). Later opens are the
    // PREVIOUS completed seed's average, including throughout partial updates.
    const std::array<Bar, 9> expected{{
        {12, 18, 8, 12.5, 3, 0},
        {11, 20, 8, 12.5, 8, 0},
        {9, 20, 6, 11, 15, 0},
        {10, 36, 10, 31.5, 11, 180000},
        {10, 40, 10, 30.75, 24, 180000},
        {10, 40, 10, 28, 41, 180000},
        {19, 19, 2, 5, 19, 360000},
        {19, 19, 1, 5.25, 42, 360000},
        {19, 19, 1, 7, 71, 360000},
    }};
    const std::array<double, 9> seed_open{{0, 0, 9, 9, 9, 10, 10, 10, 19}};
    const std::array<double, 9> seed_close{{0, 0, 11, 11, 11, 28, 28, 28, 7}};
    for (bool lookahead : {false, true}) {
        auto cursor = host.make_runtime_security_cursor("3", lookahead, false, false, true);
        Capture out;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            out.feed(cursor, inputs[i], i);
            require(out.rows.size() == (lookahead ? i + 1 : (i + 1) / 3),
                    "HA callback count");
            require(cursor.ha_seeded == (i >= 2), "HA seed completion flag");
            number(cursor.ha_open, seed_open[i], "HA completed seed open");
            number(cursor.ha_close, seed_close[i], "HA completed seed close");
            require(cursor.sub_bar_index == 0, "HA scalar count");
            if (!lookahead && i < 2) {
                number(out.latest, std::numeric_limits<double>::quiet_NaN(),
                       "HA incomplete result is absent");
                require(cursor.requested_index == -1, "HA no slot before completion");
            } else {
                const auto last_input = lookahead ? i : ((i + 1) / 3) * 3 - 1;
                number(out.latest, expected[last_input].close, "HA latest delivered value");
                require(cursor.requested_index == static_cast<std::int64_t>(last_input / 3),
                        "HA requested index after every input");
                require(cursor.slot_open == expected[last_input].timestamp,
                        "HA requested slot timestamp");
            }
            if (lookahead || i % 3 == 2) {
                row(out.rows.back(), expected[i], i % 3 == 2,
                    !lookahead || i % 3 == 0, static_cast<std::int64_t>(i / 3),
                    0, i < 6 ? 0 : 360000, "HA row " + std::to_string(i));
            }
        }
        require(out.starts == std::vector<std::int64_t>({0, 360000}),
                "HA chart clock is independent of requested clock");
        require(out.begin_positions == (lookahead ? std::vector<std::size_t>{0, 6}
                                                   : std::vector<std::size_t>{0, 2}),
                "HA chart begin order");
    }
}

// IMPLEMENTATION DETAIL: a fresh cursor starts at its first observed input.
// Keep these values pending an independent off-boundary TV witness.
void handoff_switch_and_factory_reset() {
    Host host;
    host.prepare("1", "6");
    auto original = host.make_runtime_security_cursor("3", true, false, false, true);
    Capture continued;
    continued.feed(original, inputs[0], 0);
    require(continued.rows.size() == 1, "handoff first partial count");
    row(continued.rows[0], {12, 18, 8, 12.5, 3, 0},
        false, true, 0, 0, 0, "handoff first partial");
    continued.feed(original, inputs[1], 1);
    require(continued.rows.size() == 2, "handoff second partial count");
    row(continued.rows[1], {11, 20, 8, 12.5, 8, 0},
        false, false, 0, 0, 0, "handoff second partial");
    auto handed_off = std::move(original);
    continued.feed(handed_off, inputs[2], 2);
    require(continued.rows.size() == 3 && continued.starts.size() == 1,
            "move handoff retains partial clock");
    row(continued.rows.back(), {9, 20, 6, 11, 15, 0}, true, false, 0, 0, 0,
        "handoff completion");
    require(handed_off.ha_seeded, "handoff seeds on completion");

    auto switched = host.make_runtime_security_cursor("1", false, false, false);
    pristine(switched, "1", "6", false, false, false, false);
    Capture other;
    other.feed(switched, inputs[3], 3);
    require(other.rows.size() == 1 && other.starts == std::vector<std::int64_t>({180000}),
            "new request starts its own observed chart");
    row(other.rows[0], inputs[3], true, true, 0, 0, 180000, "switched clock");
    continued.feed(handed_off, inputs[3], 3);
    require(continued.rows.size() == 4 && continued.starts.size() == 1,
            "original clock has one new partial after switch");
    row(continued.rows.back(), {10, 36, 10, 31.5, 11, 180000},
        false, true, 1, 0, 0, "original clock survives switch");

    host.prepare("1", "3");
    auto fresh = host.make_runtime_security_cursor("", true, false, false, true);
    pristine(fresh, "3", "3", true, false, false, true);
    Capture restarted;
    restarted.feed(fresh, inputs[3], 0);
    require(restarted.rows.size() == 1
                && restarted.starts == std::vector<std::int64_t>({180000}),
            "fresh factory begins one independent chart");
    row(restarted.rows.at(0), {32, 36, 26, 31.5, 11, 180000},
        false, true, 0, 0, 180000, "fresh factory resets HA seed and index");
    require(!fresh.ha_seeded && fresh.ha_open == 0 && fresh.ha_close == 0,
            "fresh partial has no inherited HA seed");
}

// IMPLEMENTATION DETAIL: scope/TA restoration on delivery unwind.
void throwing_delivery_restores_context() {
    Host host;
    host.prepare("1", "6");
    auto cursor = host.make_runtime_security_cursor("1", false, false, false);
    ta::BarContextScope caller(81, 27);
    bool began = false, delivered = false, caught = false;
    try {
        PineRuntimeSecurityScope scope(host, cursor);
        cursor.feed({inputs[0], 0, 1}, [&](const Bar& bar, bool complete) {
            delivered = true;
            equal_bar(bar, inputs[0], "throwing delivery subject");
            require(began && complete && cursor.slot_new && cursor.requested_index == 0,
                    "throwing delivery publication fields");
            const auto context = ta::bar_context();
            require(context.installed && context.bar_index == 0 && context.origin == 0,
                    "throwing delivery requested TA context");
            require(PineRuntimeSecurityScope::current(host) == &cursor,
                    "throwing delivery scope identity");
            throw Unwind{};
        }, [&] { began = true; });
    } catch (const Unwind&) { caught = true; }
    require(began && delivered && caught, "delivery exception must propagate");
    const auto restored = ta::bar_context();
    require(restored.installed && restored.bar_index == 81 && restored.origin == 27,
            "throwing delivery TA unwind");
    require(PineRuntimeSecurityScope::current(host) == nullptr, "throwing scope unwind");
}
} // namespace

int main() {
    try {
        factory_validation();
        inherited_scopes_and_identity();
        chart_boundaries_and_publication();
        lower_arrays_every_element();
        subdivision_every_element_and_order();
        no_loaded_bars_has_no_publication();
        heikin_ashi_complete_and_partial();
        handoff_switch_and_factory_reset();
        throwing_delivery_restores_context();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "runtime security cursor failure: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "runtime security cursor failure: unexpected exception\n");
        return 1;
    }
    std::puts("runtime security cursor: all rows completed");
    return 0;
}
