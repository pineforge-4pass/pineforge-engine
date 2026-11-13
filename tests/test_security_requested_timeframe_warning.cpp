#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/execution_consumer.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "../runner/json.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
using pineforge::live::Json;

constexpr std::int64_t start_ms = INT64_C(1704067200000);
constexpr std::int64_t hour_ms = INT64_C(3600000);

// Shared with the request_timeframe_not_multiple end-to-end run_json row:
// its fixture returns this exact array before the runner parses it.
constexpr const char* single_warning =
    "[{\"args\":{\"input_tf\":\"60\",\"requested_tf\":\"75\",\"sec_id\":0},"
    "\"code\":\"request_timeframe_not_multiple\"}]";

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

[[maybe_unused]] pf_strategy_t handle(BacktestEngine& host) { return &host; }

struct Warnings {
    bool carrier = false;
    std::string text = "[]";
    std::vector<Json> elements;
};

Warnings warnings(BacktestEngine& host) {
    Warnings result;
    const int status = host.last_run_status();
    const std::string failure_text = host.last_error();
    const std::string failure_code = run_failure_code_of(host);
    const std::string failure_args = run_failure_args_of(host);
#ifdef PINEFORGE_HAS_RUN_WARNINGS_V1
    const char* text = strategy_get_run_warnings(handle(host));
    require(text != nullptr, "a valid handle returned a NULL warning string");
    result.carrier = true;
    result.text = text;
#else
    // Older headers expose no warning carrier. The real run still executes,
    // and every case below checks its observed element count.
    (void)host;
#endif
    require(host.last_run_status() == status && host.last_error() == failure_text
                && run_failure_code_of(host) == failure_code
                && run_failure_args_of(host) == failure_args,
            "warning reader changed execution status or failure diagnostics");
    auto parsed = pineforge::live::parse_json(result.text);
    require(parsed.kind == Json::Kind::Array, "warnings are not a JSON array");
    require(parsed.dump() == result.text,
            "warning JSON is not compact UTF-8 with sorted object keys");
    for (const auto& element : parsed.items) {
        require(element.kind == Json::Kind::Object && element.members.size() == 2,
                "a warning must contain exactly args and code");
        require(element.at("args").kind == Json::Kind::Object,
                "warning args are not an object");
        const auto code = element.at("code").text();
        require(code.size() >= 3 && code.size() <= 48
                    && code.front() >= 'a' && code.front() <= 'z',
                "warning code has an invalid length or first character");
        for (const char c : code) {
            require((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_',
                    "warning code is not lower snake_case ASCII");
        }
    }
    result.elements = std::move(parsed.items);
    return result;
}

void expect_count(const Warnings& observed, std::size_t count) {
    require(observed.elements.size() == count,
            "warning count: expected " + std::to_string(count) + ", got "
                + std::to_string(observed.elements.size()));
}

void expect_site(const Json& warning, int sec_id, const std::string& requested,
                 const std::string& input) {
    require(warning.at("code").text() == "request_timeframe_not_multiple",
            "wrong warning code");
    const auto& args = warning.at("args");
    require(args.members.size() == 3, "wrong warning argument count");
    require(args.at("sec_id").integer<int>() == sec_id, "wrong warning site");
    require(args.at("requested_tf").text() == requested,
            "wrong requested timeframe literal");
    require(args.at("input_tf").text() == input, "wrong evaluator feed literal");
}

struct Report {
    ReportC value{};
    ~Report() { BacktestEngine::free_report(&value); }
};

class SecurityHost final : public source::PineStrategyHost {
public:
    std::vector<std::pair<int, std::string>> requests;
    bool lower_tf_array = false;
    bool place_orders = false;
    int calculations = 0;
    std::vector<std::vector<double>> delivered;
    std::vector<double> observed_values;
    std::vector<int> observed_quantities;

    explicit SecurityHost(
            std::vector<std::pair<int, std::string>> sites = {{0, "75"}},
            bool lower = false, bool orders = false)
        : requests(std::move(sites)), lower_tf_array(lower), place_orders(orders) {}

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        delivered.assign(requests.size(), {});
        current_.assign(requests.size(), std::numeric_limits<double>::quiet_NaN());
        observed_values.clear();
        observed_quantities.clear();
        calculations = 0;
        for (const auto& site : requests) {
            if (lower_tf_array) {
                register_security_lower_tf_eval(site.first, site.second, input_tf_);
            } else {
                register_security_eval(site.first, site.second, input_tf_, false, false);
            }
        }
    }

    void evaluate_security(int sec_id, const Bar& bar, bool publish) override {
        if (!publish) return;
        for (std::size_t i = 0; i < requests.size(); ++i) {
            if (requests[i].first != sec_id) continue;
            delivered[i].push_back(bar.close);
            current_[i] = bar.close;
            return;
        }
        throw std::runtime_error("evaluation named an unregistered security site");
    }

    void on_source_bar(const Bar&) override {
        ++calculations;
        if (current_.empty()) return;
        observed_values.push_back(current_.front());
        if (!std::isfinite(current_.front())) return;
        const int quantity = 1 + static_cast<int>(current_.front()) % 5;
        observed_quantities.push_back(quantity);
        if (!place_orders) return;
        // The fixed close request and order body of the selected-timeframe script.
        if (bar_index_ > 6 && bar_index_ % 7 == 3) {
            const double unset = std::numeric_limits<double>::quiet_NaN();
            strategy_entry("L", true, unset, unset, quantity);
        }
        if (bar_index_ > 6 && bar_index_ % 7 == 5) strategy_close("L");
    }

private:
    std::vector<double> current_;
};

std::vector<Bar> tape(int count, std::int64_t step = hour_ms) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i) {
        const double price = 100.0 + i;
        bars.push_back({price, price + 0.5, price - 0.5, price, 1000.0,
                        start_ms + i * step});
    }
    return bars;
}

void run_success(SecurityHost& host, const std::vector<Bar>& bars,
                 const std::string& input = "60", const std::string& chart = "60") {
    host.run(bars.data(), static_cast<int>(bars.size()), input, chart);
    require(host.last_run_status() == 0 && host.last_error().empty(),
            "run failed: " + host.last_error());
    require(host.calculations > 0, "run did not evaluate the chart body");
    Report report;
    host.fill_report(&report.value);
    require(report.value.input_bars_processed == static_cast<std::int64_t>(bars.size()),
            "report did not consume the supplied feed");
}

void single_site() {
    SecurityHost host({{0, "75"}}, false, true);
    run_success(host, tape(24));
    require(host.trade_count() > 0, "the selected-timeframe order body did not trade");
    const auto observed = warnings(host);
    expect_count(observed, 1);
    expect_site(observed.elements[0], 0, "75", "60");
    require(observed.text == single_warning, "single-site canonical warning changed");

    SecurityHost wider({{0, "130"}});
    run_success(wider, tape(24));
    const auto wider_warning = warnings(wider);
    expect_count(wider_warning, 1);
    expect_site(wider_warning.elements[0], 0, "130", "60");
}

void multiple() {
    SecurityHost host({{0, "120"}});
    run_success(host, tape(24));
    require(!host.delivered[0].empty(), "multiple timeframe was not evaluated");
    expect_count(warnings(host), 0);
}

void same_timeframe() {
    SecurityHost host({{0, "60"}});
    run_success(host, tape(24));
    require(host.delivered[0].size() == 24, "same-timeframe delivery changed");
    expect_count(warnings(host), 0);
}

void compatible_calendar() {
    for (const std::string requested : {"D", "W", "M"}) {
        SecurityHost host({{0, requested}});
        run_success(host, tape(24));
        expect_count(warnings(host), 0);
    }
    SecurityHost daily({{0, "W"}});
    run_success(daily, tape(7, 24 * hour_ms), "D", "D");
    expect_count(warnings(daily), 0);
}

void two_sites() {
    // Register in reverse identity order to exercise canonical element ordering.
    SecurityHost host({{1, "90"}, {0, "75"}});
    run_success(host, tape(24));
    require(!host.delivered[0].empty() && !host.delivered[1].empty(),
            "both requested sites must be evaluated");
    const auto observed = warnings(host);
    expect_count(observed, 2);
    expect_site(observed.elements[0], 0, "75", "60");
    expect_site(observed.elements[1], 1, "90", "60");
    require(observed.text ==
                "[{\"args\":{\"input_tf\":\"60\",\"requested_tf\":\"75\",\"sec_id\":0},"
                "\"code\":\"request_timeframe_not_multiple\"},"
                "{\"args\":{\"input_tf\":\"60\",\"requested_tf\":\"90\",\"sec_id\":1},"
                "\"code\":\"request_timeframe_not_multiple\"}]",
            "two-site canonical warning text changed");
}

void many_bars() {
    SecurityHost host({{0, "75"}}, false, true);
    run_success(host, tape(80));
    require(host.calculations == 80 && host.delivered[0].size() == 80,
            "fixture did not evaluate the same site on all eighty bars");
    const auto observed = warnings(host);
    expect_count(observed, 1);
    expect_site(observed.elements[0], 0, "75", "60");
}

void unchanged_values() {
    SecurityHost host({{0, "75"}}, false, true);
    run_success(host, tape(64));
    require(host.delivered[0].size() == 64 && host.observed_values.size() == 64
                && host.observed_quantities.size() == 64,
            "the evaluator or caller dropped a value");
    // The existing passthrough evaluator delivers each hourly input close.
    for (int i = 0; i < 64; ++i) {
        const auto index = static_cast<std::size_t>(i);
        require(host.delivered[0][index] == 100.0 + i
                    && host.observed_values[index] == 100.0 + i,
                "warning changed a requested close or its calling-bar publication");
        require(host.observed_quantities[index] == 1 + (100 + i) % 5,
                "warning changed the quantity derived from the requested close");
    }
    const auto observed = warnings(host);
    expect_count(observed, observed.carrier ? 1 : 0);
}

void auxiliary_feed() {
    SecurityHost host;
    const auto auxiliary = tape(96, hour_ms / 4);
    require(host.set_aux_security_feed(auxiliary.data(),
                static_cast<int>(auxiliary.size()), "15"),
            "auxiliary feed was refused");
    run_success(host, tape(24));
    require(!host.delivered[0].empty(), "auxiliary evaluator never published");
    expect_count(warnings(host), 0);
}

void silent_kinds() {
    const auto bars = tape(24);
    SecurityHost array({{0, "15"}}, true);
    run_success(array, bars);
    require(!array.delivered[0].empty(), "lower-timeframe array was not evaluated");
    expect_count(warnings(array), 0);

    SecurityHost unloaded({{0, "15"}});
    run_success(unloaded, bars, "60", "120");
    require(unloaded.delivered[0].empty(), "unloaded lower timeframe was evaluated");
    for (const double value : unloaded.observed_values)
        require(std::isnan(value), "unloaded lower timeframe did not retain na");
    expect_count(warnings(unloaded), 0);

    SecurityHost refused({{0, "15"}});
    refused.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
    require(!refused.last_error().empty()
                && run_failure_value_of(refused).code
                    == RunFailureCode::request_timeframe_unsupported,
            "finer-than-input scalar request lost its existing refusal");
    expect_count(warnings(refused), 0);
}

void refused_attempt() {
    SecurityHost host;
    const auto bars = tape(24);
    run_success(host, bars);
    const auto first = warnings(host);
    expect_count(first, first.carrier ? 1 : 0);
    auto invalid = bars;
    invalid[1].timestamp = invalid[0].timestamp;
    host.run(invalid.data(), static_cast<int>(invalid.size()), "60", "60");
    require(!host.last_error().empty(), "unordered feed was accepted");
    require(host.calculations == 24, "refused feed entered a new source run");
    Report report;
    host.fill_report(&report.value);
    require(report.value.input_bars_processed == 0 && report.value.trades_len == 0,
            "refused attempt exposed the previous report");
    expect_count(warnings(host), 0);
}

void live_stream() {
    SecurityHost host;
    const auto bars = tape(8);
    require(host.stream_begin(bars.data(), static_cast<int>(bars.size()), "60", "60"),
            "stream begin failed: " + host.last_error());
    const auto first = warnings(host);
    expect_count(first, first.carrier ? 1 : 0);
    host.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
    require(!host.last_error().empty() && host.stream_is_realtime(),
            "batch call was not refused over the live stream");
    const auto retained = warnings(host);
    expect_count(retained, retained.carrier ? 1 : 0);
    if (retained.carrier) expect_site(retained.elements[0], 0, "75", "60");
    // Cleanup only: whether a stream can still end after a refused call is not
    // what this row pins (the engine answers false there with or without the
    // warnings), so the result is not asserted.
    host.stream_end();
}

class UnboundQueryHost final : public BacktestEngine {
public:
    UnboundQueryHost() : BacktestEngine(NativeConsumerBindTag{}) {
        // Discard the eager native binding before any run to exercise the
        // reader's empty-slot path without a source adapter.
        execution_consumer_slot_.ptr.reset();
    }
    bool consumer_present() const { return execution_consumer_slot_.ptr != nullptr; }
    void on_bar(const Bar&) override {}
};

void c_contract() {
#ifdef PINEFORGE_HAS_RUN_WARNINGS_V1
    require(strategy_get_run_warnings(nullptr) == nullptr,
            "NULL handle did not return NULL");
#endif
    UnboundQueryHost unbound;
    require(!unbound.consumer_present(), "query fixture has a consumer");
    expect_count(warnings(unbound), 0);
    require(!unbound.consumer_present(), "warning reader created a consumer");
    SecurityHost host;
    SecurityHost other;
    expect_count(warnings(host), 0);
    const auto bars = tape(24);
    run_success(host, bars);
    const auto first = warnings(host);
    expect_count(first, first.carrier ? 1 : 0);
    expect_count(warnings(other), 0);
#ifdef PINEFORGE_HAS_RUN_WARNINGS_V1
    const char* borrowed = strategy_get_run_warnings(handle(host));
    const std::string saved = borrowed;
    require(std::string(strategy_get_run_warnings(handle(host))) == saved,
            "second read changed the warning string");
    Report report;
    host.fill_report(&report.value);
    require(std::string(borrowed) == saved
                && std::string(strategy_get_run_warnings(handle(host))) == saved,
            "report read invalidated the borrowed warning string");
#endif
    host.requests = {{0, "120"}};
    run_success(host, bars);
    expect_count(warnings(host), 0);
    host.requests = {{0, "75"}};
    run_success(host, bars);
    const auto before_stream = warnings(host);
    expect_count(before_stream, before_stream.carrier ? 1 : 0);
    host.requests = {{0, "120"}};
    require(host.stream_begin(bars.data(), static_cast<int>(bars.size()), "60", "60"),
            "stream reset failed: " + host.last_error());
    expect_count(warnings(host), 0);
    require(host.stream_end(), "reset stream did not end");
}

} // namespace

int main(int argc, char** argv) {
    const std::pair<const char*, void (*)()> cases[] = {
        {"single_site", single_site}, {"multiple", multiple},
        {"same_timeframe", same_timeframe}, {"compatible_calendar", compatible_calendar},
        {"two_sites", two_sites},
        {"many_bars", many_bars}, {"unchanged_values", unchanged_values},
        {"auxiliary_feed", auxiliary_feed}, {"silent_kinds", silent_kinds},
        {"refused_attempt", refused_attempt}, {"live_stream", live_stream},
        {"c_contract", c_contract},
    };
    int failures = 0;
    bool selected = false;
    for (const auto& row : cases) {
        if (argc == 2 && std::string(argv[1]) != row.first) continue;
        if (argc > 2) break;
        selected = true;
        try {
            row.second();
            std::printf("%s: PASS\n", row.first);
        } catch (const std::exception& error) {
            ++failures;
            std::fprintf(stderr, "%s: %s\n", row.first, error.what());
        }
    }
    if (!selected) {
        std::fprintf(stderr, "expected no argument or one known case name\n");
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
