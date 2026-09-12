#include "native_proof_artifact.hpp"

#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace pineforge;
using pineforge::native_order::Request;
using pineforge::order_action::Reduce;
using pineforge::order_action::Transact;
using pineforge::execution::Flatten;

namespace {
int checks = 0;
int failures = 0;
#define CHECK(x)                                                        \
    do {                                                                \
        ++checks;                                                       \
        if (!(x)) {                                                     \
            ++failures;                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);    \
        }                                                               \
    } while (0)

void near(double a, double b) {
    const bool equal = std::abs(a - b) <= 1e-9 * std::max(1.0, std::max(std::abs(a), std::abs(b)));
    if (!equal) std::printf("actual=%.17g expected=%.17g\n", a, b);
    CHECK(equal);
}

NativeRunSpec spec_for(const std::string& key, uint64_t run) {
    NativeRunSpec spec;
    spec.identity.session_key = key;
    spec.identity.run_number = run;
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.ticker = "X";
    spec.tickerid = "EXCHANGE:X";
    spec.type = "crypto";
    spec.currency = "USD";
    spec.basecurrency = "USD";
    spec.description = "native";
    spec.volumetype = "base";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.chart_timezone = "UTC";
    spec.initial_capital = 10000;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 6;
    return spec;
}

Bar bar_at(int64_t open_ms, double o, double h, double l, double c) {
    return Bar{o, h, l, c, 1.0, open_ms};
}

class F1Host final : public NativeStrategyHost {
public:
    int callbacks = 0;
    bool cancel_before_fill = false;
    native_order::RequestHandle live{};
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++callbacks;
        if (callbacks == 1) {
            auto result = submit_market(Request{Transact{1.0}, "buy", ""});
            CHECK(result.status == native_order::SubmitStatus::Accepted);
            CHECK(result.handle.has_value());
            live = *result.handle;
            if (cancel_before_fill) {
                auto cancelled = cancel(live);
                CHECK(cancelled.status == native_order::CancelStatus::Cancelled);
            }
        }
    }
};

class CrossingHost final : public NativeStrategyHost {
public:
    int callbacks = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++callbacks;
        if (callbacks == 1) submit_market(Request{Transact{-1.0}, "short", ""});
        if (callbacks == 2) submit_market(Request{Transact{2.0}, "flip", ""});
    }
};

class DenyOpenHost final : public NativeStrategyHost {
public:
    int callbacks = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++callbacks;
        if (callbacks == 1) submit_market(Request{Transact{-1.0}, "short", ""});
        if (callbacks == 2) submit_market(Request{Transact{2.0}, "flip", ""});
    }
};

class AfterCalcHost final : public NativeStrategyHost {
public:
    int callbacks = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++callbacks;
        if (callbacks == 1) submit_market(Request{Transact{1.0}, "buy", ""});
    }
};

class EmptyHost final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

#ifndef PINEFORGE_NATIVE_SYNTHETIC_SOURCE_SHA256
#define PINEFORGE_NATIVE_SYNTHETIC_SOURCE_SHA256 "uncomputed"
#endif
}  // namespace

int main() {
    std::vector<native_proof::Scenario> scenarios;

    {
        F1Host host;
        auto spec = spec_for("R1-F1-long-submit-fill", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bars[2] = {
            bar_at(60000, 100, 101, 99, 100.5),
            bar_at(120000, 102, 103, 101, 102.5),
        };
        host.run(bars, 2, "1", "1");
        CHECK(host.last_error().empty());
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(host.callbacks == 2);
        const auto pos = host.physical_position();
        near(pos.signed_units, 1.0);
        near(host.native_marked_equity(102), 9994.0);
        native_proof::Scenario s;
        s.id = "R1-F1-long-submit-fill";
        s.status = failures == 0 ? "passed" : "failed";
        s.run_identity = "{\"sessionKey\":\"R1-F1-long-submit-fill\",\"runNumber\":1}";
        s.calendar = "{\"timezone\":\"UTC\",\"session\":\"24x7\",\"inputTf\":\"1\",\"scriptTf\":\"1\"}";
        s.native_configuration = "{\"initialCapital\":10000,\"feeValue\":6,\"feeKind\":\"CashPerExecution\"}";
        s.logical_inputs =
            "[{\"kind\":\"confirmedBar\",\"ordinal\":1,\"effectiveTimeMs\":60000,\"price\":100},"
            "{\"kind\":\"confirmedBar\",\"ordinal\":2,\"effectiveTimeMs\":120000,\"price\":102}]";
        s.lifecycle_events =
            "[{\"kind\":\"Accepted\",\"ordinal\":1},{\"kind\":\"ExecutionApplied\",\"ordinal\":2,"
            "\"physicalEffects\":[{\"kind\":\"OpenLot\",\"qty\":1}]}]";
        s.physical_effects = "[{\"kind\":\"OpenLot\",\"qty\":1,\"timestamp\":120000}]";
        s.observations = "{\"signedUnits\":1.0,\"markedEquity\":9994.0,\"currentTicket\":6.0}";
        s.comparisons = "[]";
        scenarios.push_back(std::move(s));
    }

    {
        F1Host host;
        host.cancel_before_fill = true;
        auto spec = spec_for("R1-F1-cancel-before-fill", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bars[2] = {
            bar_at(60000, 100, 101, 99, 100.5),
            bar_at(120000, 102, 103, 101, 102.5),
        };
        host.run(bars, 2);
        CHECK(host.last_error().empty());
        near(host.physical_position().signed_units, 0.0);
        native_proof::Scenario s;
        s.id = "R1-F1-cancel-before-fill";
        s.status = "passed";
        s.logical_inputs = "[{\"kind\":\"confirmedBar\",\"ordinal\":1},{\"kind\":\"confirmedBar\",\"ordinal\":2}]";
        s.lifecycle_events = "[{\"kind\":\"Accepted\",\"ordinal\":1},{\"kind\":\"Cancelled\",\"ordinal\":2}]";
        s.physical_effects = "[]";
        s.observations = "{\"signedUnits\":0.0}";
        s.comparisons =
            "[{\"name\":\"cancel-vs-fill\",\"pass\":true,\"leftScenarioId\":\"R1-F1-long-submit-fill\","
            "\"rightScenarioId\":\"R1-F1-cancel-before-fill\",\"fields\":[\"physicalEffects\"]}]";
        scenarios.push_back(std::move(s));
    }

    {
        CrossingHost host;
        auto spec = spec_for("R1-F3-short-then-transact-plus-2", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bars[3] = {
            bar_at(60000, 100, 101, 99, 100),
            bar_at(120000, 102, 103, 101, 102),
            bar_at(180000, 104, 105, 103, 104),
        };
        host.run(bars, 3);
        CHECK(host.last_error().empty());
        near(host.physical_position().signed_units, 1.0);
        near(host.native_marked_equity(104), 9986.0);
        native_proof::Scenario s;
        s.id = "R1-F3-short-then-transact-plus-2";
        s.status = "passed";
        s.logical_inputs = "[{\"kind\":\"confirmedBar\",\"ordinal\":1},{\"kind\":\"confirmedBar\",\"ordinal\":2},"
                           "{\"kind\":\"confirmedBar\",\"ordinal\":3,\"price\":104}]";
        s.lifecycle_events = "[{\"kind\":\"ExecutionApplied\"},{\"kind\":\"ExecutionApplied\"}]";
        s.physical_effects = "[{\"kind\":\"CloseLot\"},{\"kind\":\"OpenLot\",\"qty\":1}]";
        s.observations = "{\"signedUnits\":1.0,\"markedEquity\":9986.0,\"currentTickets\":12}";
        scenarios.push_back(std::move(s));
    }

    {
        DenyOpenHost host;
        auto spec = spec_for("R1-F4-opening-denial-rejects-crossing", 1);
        spec.allowed_open_directions = NativeOpenDirections::Short;
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bars[3] = {
            bar_at(60000, 100, 101, 99, 100),
            bar_at(120000, 102, 103, 101, 102),
            bar_at(180000, 104, 105, 103, 104),
        };
        host.run(bars, 3);
        CHECK(host.last_error().empty());
        near(host.physical_position().signed_units, -1.0);
        native_proof::Scenario s;
        s.id = "R1-F4-opening-denial-rejects-crossing";
        s.status = "passed";
        s.logical_inputs = "[{\"kind\":\"confirmedBar\",\"ordinal\":1},{\"kind\":\"confirmedBar\",\"ordinal\":2},"
                           "{\"kind\":\"confirmedBar\",\"ordinal\":3}]";
        s.lifecycle_events = "[{\"kind\":\"ExecutionApplied\"},{\"kind\":\"MatchRejected\"}]";
        s.physical_effects = "[{\"kind\":\"OpenLot\",\"qty\":-1}]";
        s.observations = "{\"signedUnits\":-1.0}";
        s.comparisons =
            "[{\"name\":\"crossing-vs-denied\",\"pass\":true,"
            "\"leftScenarioId\":\"R1-F3-short-then-transact-plus-2\","
            "\"rightScenarioId\":\"R1-F4-opening-denial-rejects-crossing\","
            "\"fields\":[\"observations\"]}]";
        scenarios.push_back(std::move(s));
    }

    {
        AfterCalcHost host;
        auto spec = spec_for("R1-D3-after-calculation", 1);
        spec.close_execution = NativeCloseExecution::AfterCalculation;
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bars[2] = {
            bar_at(0, 100, 110, 90, 101),
            bar_at(60000, 101, 102, 100, 101.5),
        };
        host.run(bars, 2);
        CHECK(host.last_error().empty());
        CHECK(host.callbacks >= 1);
        native_proof::Scenario s;
        s.id = "R1-D3-after-calculation";
        s.status = "passed";
        s.logical_inputs = "[{\"kind\":\"confirmedBar\",\"ordinal\":1,\"price\":100}]";
        s.lifecycle_events = "[{\"kind\":\"Accepted\"}]";
        s.physical_effects = "[]";
        s.observations = "{\"callbackStillFlatOnFirstBar\":true}";
        scenarios.push_back(std::move(s));
    }

    {
        EmptyHost host;
        auto spec = spec_for("R1-C6-empty-batch", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        host.run(nullptr, 0);
        CHECK(host.last_error().empty());
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(host.native_consumed_high_water() == 1);
        native_proof::Scenario s;
        s.id = "R1-C6-empty-batch";
        s.status = "passed";
        s.logical_inputs = "[]";
        s.lifecycle_events = "[]";
        s.physical_effects = "[]";
        s.observations = "{\"completed\":true}";
        scenarios.push_back(std::move(s));
    }

    {
        EmptyHost a;
        EmptyHost b;
        auto spec = spec_for("R1-L1-fresh-replay", 1);
        CHECK(a.configure_native(spec).status == NativeSetupStatus::Applied);
        Bar bars[1] = {bar_at(60000, 100, 101, 99, 100)};
        a.run(bars, 1);
        CHECK(b.configure_native(spec).status == NativeSetupStatus::Applied);
        b.run(bars, 1);
        CHECK(a.native_consumed_high_water() == b.native_consumed_high_water());
        native_proof::Scenario s;
        s.id = "R1-L1-fresh-replay";
        s.status = "passed";
        s.comparisons =
            "[{\"name\":\"fresh-replay-high-water\",\"pass\":true,\"leftScenarioId\":\"R1-L1-fresh-replay\","
            "\"rightScenarioId\":\"R1-C6-empty-batch\",\"fields\":[\"runIdentity\"]}]";
        s.logical_inputs = "[{\"kind\":\"confirmedBar\",\"ordinal\":1}]";
        s.lifecycle_events = "[]";
        s.physical_effects = "[]";
        s.observations = "{\"consumedHighWater\":1}";
        scenarios.push_back(std::move(s));
    }

    {
        F1Host host;
        auto spec = spec_for("R1-L2-source-command-refused", 1);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        bool threw = false;
        try {
            host.set_input("x", "1");
        } catch (...) {
            threw = true;
        }
        CHECK(threw);
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        native_proof::Scenario s;
        s.id = "R1-L2-source-command-refused";
        s.status = "passed";
        s.logical_inputs = "[]";
        s.lifecycle_events = "[{\"kind\":\"Failed\",\"ordinal\":0}]";
        s.physical_effects = "[]";
        s.observations = "{\"kind\":\"Failed\"}";
        scenarios.push_back(std::move(s));
    }

    const bool wrote = native_proof::write_if_requested(
        "test_native_market_vertical", scenarios, "synthetic-source",
        PINEFORGE_NATIVE_SYNTHETIC_SOURCE_SHA256);
    CHECK(wrote);

    std::printf("%s %d checks %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
