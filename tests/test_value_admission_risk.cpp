// Authored before the producers. Only existing host/intent APIs are
// used, so the failure rows compile against the previous header closure and
// fail there because unchecked risk values are stored/replayed/restored.
#include <pineforge/run_failure.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/source/pine_intent_state.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v19 {
int native_set_selected_window_v1(BacktestEngine*, const pf_selected_window_config_v1*) noexcept;
}
}

using namespace pineforge;
namespace intent = pineforge::source::detail;

namespace {

int checks = 0;
int failures = 0;
int rows_run = 0;
const char* current_row = "setup";
const char* only_row = nullptr;

#define CHECK(condition) do {                                                   \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        ++failures;                                                            \
        std::fprintf(stderr, "FAIL [%s] %s:%d: %s\n", current_row, __FILE__,     \
                     __LINE__, #condition);                                    \
    }                                                                          \
} while (0)

constexpr std::int64_t kT0 = 1736121600000LL;
constexpr std::int64_t kMinute = 60000;

enum class Limit { Drawdown, IntradayLoss, PositionSize };
struct LimitCase {
    Limit limit;
    const char* label;
    const char* field;
};
constexpr LimitCase kLimits[] = {
    {Limit::Drawdown, "MAX_DRAWDOWN", "risk.max_drawdown"},
    {Limit::IntradayLoss, "MAX_INTRADAY_LOSS", "risk.max_intraday_loss"},
    {Limit::PositionSize, "MAX_POSITION_SIZE", "risk.max_position_size"},
};

struct ValueCase {
    const char* label;
    double value;
    bool invalid;
};
const ValueCase kValues[] = {
    {"POS_INF", std::numeric_limits<double>::infinity(), true},
    {"NEG_INF", -std::numeric_limits<double>::infinity(), true},
    {"NA", std::numeric_limits<double>::quiet_NaN(), false},
    {"FINITE", 12.5, false},
    {"ZERO", 0.0, false},
    {"NEGATIVE", -12.5, false},
};

Bar bar_at(int index) {
    return {100.0, 100.0, 100.0, 100.0, 1.0, kT0 + index * kMinute};
}

std::vector<Bar> tape() {
    std::vector<Bar> bars;
    for (int i = 0; i < 6; ++i) bars.push_back(bar_at(i));
    return bars;
}

bool same_number(double actual, double expected) {
    return std::isnan(expected) ? std::isnan(actual) : actual == expected;
}

class Fixture final : public source::PineStrategyHost {
public:
    using Script = std::function<void(Fixture&, const Bar&)>;

    explicit Fixture(Script script) : script_(std::move(script)) {
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_value = 1.0;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar& bar) override {
        callbacks.push_back(bar.timestamp);
        script_(*this, bar);
    }

    source::PineReplayRiskConfig risk() const {
        return adapter_.capture_replay_risk_config();
    }

    std::vector<std::int64_t> callbacks;

private:
    Script script_;
};

void set_limit(Fixture& host, Limit limit, double value, bool percent) {
    switch (limit) {
    case Limit::Drawdown: host.set_pine_risk_max_drawdown(value, percent); break;
    case Limit::IntradayLoss: host.set_pine_risk_max_intraday_loss(value, percent); break;
    case Limit::PositionSize: host.set_pine_risk_max_position_size(value); break;
    }
}

double read_limit(const source::PineReplayRiskConfig& snapshot, Limit limit) {
    switch (limit) {
    case Limit::Drawdown: return snapshot.max_drawdown;
    case Limit::IntradayLoss: return snapshot.max_intraday_loss;
    case Limit::PositionSize: return snapshot.max_position_size;
    }
    throw std::logic_error("unknown test limit");
}

void write_limit(source::PineReplayRiskConfig& snapshot, Limit limit, double value) {
    switch (limit) {
    case Limit::Drawdown: snapshot.max_drawdown = value; break;
    case Limit::IntradayLoss: snapshot.max_intraday_loss = value; break;
    case Limit::PositionSize: snapshot.max_position_size = value; break;
    }
}

void check_same_risk(const source::PineReplayRiskConfig& actual,
                     const source::PineReplayRiskConfig& expected) {
    CHECK(actual.direction == expected.direction);
    CHECK(actual.max_cons_loss_days == expected.max_cons_loss_days);
    CHECK(same_number(actual.max_drawdown, expected.max_drawdown));
    CHECK(actual.max_drawdown_percent == expected.max_drawdown_percent);
    CHECK(same_number(actual.max_intraday_loss, expected.max_intraday_loss));
    CHECK(actual.max_intraday_loss_percent == expected.max_intraday_loss_percent);
    CHECK(same_number(actual.max_position_size, expected.max_position_size));
    CHECK(actual.cap_attachment == expected.cap_attachment);
    CHECK(actual.cap_configuration.limit == expected.cap_configuration.limit);
    CHECK(actual.cap_configuration.skip_noop_market == expected.cap_configuration.skip_noop_market);
    CHECK(actual.cap_configuration.defer_pooc_close == expected.cap_configuration.defer_pooc_close);
    CHECK(actual.cap_configuration.count_pooc_full_close == expected.cap_configuration.count_pooc_full_close);
    CHECK(actual.cap_declared == expected.cap_declared);
}

void set_six_valid(Fixture& host) {
    host.set_pine_risk_direction(1);
    host.set_pine_risk_max_cons_loss_days(3);
    host.set_pine_risk_max_drawdown(20.0, true);
    host.set_pine_risk_max_intraday_loss(10.0, true);
    host.set_pine_risk_max_intraday_filled_orders(4);
    host.set_pine_risk_max_position_size(7.0);
}

void check_six_valid(const source::PineReplayRiskConfig& risk) {
    CHECK(risk.direction == 1);
    CHECK(risk.max_cons_loss_days == 3);
    CHECK(risk.max_drawdown == 20.0);
    CHECK(risk.max_drawdown_percent);
    CHECK(risk.max_intraday_loss == 10.0);
    CHECK(risk.max_intraday_loss_percent);
    CHECK(risk.cap_configuration.limit == 4);
    CHECK(risk.cap_attachment == compat::pine::CapAttachment::LegacySource);
    CHECK(risk.cap_declared == 0);
    CHECK(risk.cap_configuration.skip_noop_market);
    CHECK(risk.cap_configuration.defer_pooc_close);
    CHECK(risk.cap_configuration.count_pooc_full_close);
    CHECK(risk.max_position_size == 7.0);
}

void select_from(Fixture& host, int start_bar) {
    pf_selected_window_config_v1 window{};
    window.struct_size = static_cast<std::uint32_t>(sizeof window);
    window.version = 1;
    window.start_ms = bar_at(start_bar).timestamp;
    window.end_ms = bar_at(10).timestamp;
    CHECK(native_set_selected_window_v1(&host, &window) == 0);
}

void check_failed(const Fixture& host, const char* field) {
    CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
    CHECK(!host.last_error().empty());
    CHECK(std::strcmp(run_failure_code_of(host), "strategy_value_invalid") == 0);
    const std::string args = std::string("{\"field\":\"") + field
        + "\",\"reason\":\"nonfinite\"}";
    CHECK(std::strcmp(run_failure_args_of(host), args.c_str()) == 0);
    CHECK(host.trade_count() == 0);
}

void check_completed(const Fixture& host) {
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.last_error().empty());
    CHECK(std::strcmp(run_failure_code_of(host), "") == 0);
    CHECK(host.trade_count() == 0);
}

void host_row(const LimitCase& limit, const ValueCase& value,
              int start_bar, bool percent) {
    bool reached = false;
    Fixture host([&](Fixture& self, const Bar& bar) {
        if (bar.timestamp != kT0) return;
        reached = true;
        const auto before = self.risk();
        const auto* state = intent::selected_intents(self);
        const std::size_t rows_before = state ? state->rows.size() : 0;
        try {
            set_limit(self, limit.limit, value.value, percent);
        } catch (...) {
            // An invalid setter must reject before capture and before any live
            // write, including before setting a sticky percent flag.
            check_same_risk(self.risk(), before);
            if (state) CHECK(state->rows.size() == rows_before);
            throw;
        }
        CHECK(!value.invalid);
        CHECK(same_number(read_limit(self.risk(), limit.limit), value.value));
    });
    if (start_bar >= 0) select_from(host, start_bar);
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false);
    CHECK(reached);
    if (value.invalid) {
        check_failed(host, limit.field);
        CHECK(host.callbacks.size() == 1);
    } else {
        check_completed(host);
        CHECK(host.callbacks.size() == bars.size());
        CHECK(same_number(read_limit(host.risk(), limit.limit), value.value));
    }
}

// A paused real stream preserves the final pre-roll evaluation. Mutations
// below use its existing mutable source-owned state, with no new test seam.
intent::PineIntentState* begin_retained(Fixture& host) {
    select_from(host, 2);
    const Bar warmup[] = {bar_at(0), bar_at(1)};
    const bool started = host.stream_begin(warmup, 2, "1", "1");
    CHECK(started);
    if (!started) return nullptr;
    CHECK(host.last_error().empty());
    auto* state = intent::selected_intents(host);
    CHECK(state != nullptr);
    if (!state) return nullptr;
    CHECK(state->has_evaluation);
    CHECK(!state->capturing);
    CHECK(!state->replayed);
    CHECK(state->label_ms == bar_at(1).timestamp);
    CHECK(state->rows.size() == 6);
    check_six_valid(state->end_risk);
    return state;
}

bool rewrite_row(intent::PineIntentState& state, Limit limit, double value) {
    for (auto& row : state.rows) {
        if (limit == Limit::Drawdown) {
            if (auto* payload = std::get_if<intent::IntentRiskMaxDrawdown>(&row.payload)) {
                payload->value = value;
                return true;
            }
        } else if (limit == Limit::IntradayLoss) {
            if (auto* payload = std::get_if<intent::IntentRiskMaxIntradayLoss>(&row.payload)) {
                payload->value = value;
                return true;
            }
        } else if (auto* payload = std::get_if<intent::IntentRiskMaxPositionSize>(&row.payload)) {
            payload->value = value;
            return true;
        }
    }
    return false;
}

Fixture::Script retained_script() {
    return [](Fixture& self, const Bar& bar) {
        if (bar.timestamp == bar_at(1).timestamp) set_six_valid(self);
    };
}

void replay_row(const LimitCase& limit, const ValueCase& value) {
    Fixture host(retained_script());
    auto* state = begin_retained(host);
    if (!state) return;
    CHECK(rewrite_row(*state, limit.limit, value.value));
    // Both snapshots stay valid. Only the retained statement is corrupted:
    // deleting replay admission cannot be masked by snapshot admission.
    const double start_value = read_limit(state->start_risk, limit.limit);
    const auto expected_end = state->end_risk;
    const bool pushed = host.stream_push_bar(bar_at(2));
    if (value.invalid) {
        CHECK(!pushed);
        check_failed(host, limit.field);
        CHECK(same_number(read_limit(host.risk(), limit.limit), start_value));
        CHECK(host.callbacks.size() == 2);
    } else {
        CHECK(pushed);
        CHECK(state->replayed);
        CHECK(state->rows.empty());
        // The valid end snapshot wins over the row, including a NaN row.
        check_same_risk(host.risk(), expected_end);
        CHECK(host.callbacks.size() == 3);
        CHECK(host.stream_end(false));
        check_completed(host);
    }
}

void snapshot_row(const LimitCase& limit, const ValueCase& value, bool at_end) {
    Fixture host(retained_script());
    auto* state = begin_retained(host);
    if (!state) return;
    const auto admitted_end = state->end_risk;
    auto hand_built = at_end ? state->end_risk : state->start_risk;
    write_limit(hand_built, limit.limit, value.value);
    if (at_end) state->end_risk = hand_built;
    else state->start_risk = hand_built;
    const bool pushed = host.stream_push_bar(bar_at(2));
    if (value.invalid) {
        CHECK(!pushed);
        check_failed(host, limit.field);
        // Validate every double before the direct assignment: the refused
        // snapshot cannot partly overwrite the live risk configuration.
        check_same_risk(host.risk(), admitted_end);
        CHECK(host.callbacks.size() == 2);
    } else {
        CHECK(pushed);
        CHECK(state->replayed);
        CHECK(state->rows.empty());
        check_same_risk(host.risk(), at_end ? hand_built : admitted_end);
        CHECK(host.stream_end(false));
        check_completed(host);
    }
}

void six_setters_control(bool selected) {
    Fixture host([](Fixture& self, const Bar& bar) {
        if (bar.timestamp == kT0) {
            set_six_valid(self);
            check_six_valid(self.risk());
            self.set_pine_risk_max_drawdown(25.0, false);
            self.set_pine_risk_max_intraday_loss(15.0, false);
            CHECK(self.risk().max_drawdown_percent);
            CHECK(self.risk().max_intraday_loss_percent);
        }
    });
    if (selected) select_from(host, 2);
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false);
    check_completed(host);
    CHECK(host.callbacks.size() == bars.size());
    const auto risk = host.risk();
    CHECK(risk.direction == 1);
    CHECK(risk.max_cons_loss_days == 3);
    CHECK(risk.cap_configuration.limit == 4);
    CHECK(risk.max_position_size == 7.0);
    CHECK(risk.max_drawdown == 25.0 && risk.max_drawdown_percent);
    CHECK(risk.max_intraday_loss == 15.0 && risk.max_intraday_loss_percent);
}

void integer_domain_control() {
    Fixture host([](Fixture& self, const Bar&) {
        for (int value : {std::numeric_limits<int>::min(), -1, 0, 1,
                          std::numeric_limits<int>::max()}) {
            self.set_pine_risk_direction(value);
            self.set_pine_risk_max_cons_loss_days(value);
            self.set_pine_risk_max_intraday_filled_orders(value);
            const auto risk = self.risk();
            CHECK(risk.direction == value);
            CHECK(risk.max_cons_loss_days == value);
            CHECK(risk.cap_configuration.limit == value);
        }
    });
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false);
    check_completed(host);
    CHECK(host.callbacks.size() == bars.size());
}

void run_row(const std::string& label, const std::function<void()>& body) {
    if (only_row && label != only_row) return;
    current_row = label.c_str();
    ++rows_run;
    const int before_checks = checks;
    const int before_failures = failures;
    try {
        body();
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "EXCEPTION [%s]: %s\n", current_row, error.what());
    } catch (...) {
        ++failures;
        std::fprintf(stderr, "EXCEPTION [%s]: nonstandard exception\n", current_row);
    }
    const int body_checks = checks - before_checks;
    CHECK(body_checks > 0);
    std::printf("ROW %s: %s\n", current_row,
                failures == before_failures ? "PASS" : "FAIL");
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2) only_row = argv[1];
    else if (argc != 1) return 2;
    run_row("SIX-SETTERS-VALID-OFF", [] { six_setters_control(false); });
    run_row("SIX-SETTERS-VALID-PREROLL", [] { six_setters_control(true); });
    run_row("INTEGER-DOMAINS-UNCHANGED", integer_domain_control);
    for (const auto& limit : kLimits) {
        for (const auto& value : kValues) {
            const std::string key = std::string(limit.label) + "-" + value.label;
            for (int start_bar : {-1, 0, 2}) {
                const char* phase = start_bar < 0 ? "OFF" : start_bar == 0
                    ? "SELECTED" : "EARLIER-PREROLL";
                for (bool percent : {false, true}) {
                    if (limit.limit == Limit::PositionSize && percent) continue;
                    run_row("HOST-" + key + "-" + phase + (percent ? "-PERCENT" : "-CASH"),
                            [&] { host_row(limit, value, start_bar, percent); });
                }
            }
            run_row("REPLAY-" + key, [&] { replay_row(limit, value); });
            run_row("SNAPSHOT-START-" + key, [&] { snapshot_row(limit, value, false); });
            run_row("SNAPSHOT-END-" + key, [&] { snapshot_row(limit, value, true); });
        }
    }
    current_row = "summary";
    CHECK(rows_run > 0);
    std::printf("rows=%d checks=%d failures=%d\n", rows_run, checks, failures);
    return failures == 0 ? 0 : 1;
}
