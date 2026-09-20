// R5 gap lane N5 (§1.8 RP10 + RP9): a host folds its own durable state into
// the broker-state hash through the generic hash_host_extension hook, and the
// per-bar rows a KernelRecorded run records carry that fold. This unit reaches
// public headers only — BrokerStateHashSink is a complete type for an
// installed consumer — so it runs in the kernel-only build as well.
#include "native_current_fixture.hpp"

#include <pineforge/pineforge.h>

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace r4_test;

namespace {

// ── Pinned constants ────────────────────────────────────────────────────
// Observed on a clean build of engine main (683a82f, this lane's base) by a
// scratch probe carrying the fixture, the scenario and the projection below
// verbatim. They are the neutrality pin: a host that overrides nothing folds
// exactly the bytes it folded before the generic hook existed. Every operand
// is an exact binary fraction and the projection takes the execution hash as
// an argument, so the values carry no machine identity.
constexpr std::uint64_t kMainFreshBrokerState = 11785313996075156269ull;
constexpr std::uint64_t kMainRoundTripBrokerState = 10289062291417719237ull;

constexpr std::uint64_t kProbeExecutionHash = 0x5eed1234abcd0005ull;

// ── Feed, spec, rule ────────────────────────────────────────────────────
double price_at(int index) {
    const int phase = index % 20;
    const int triangle = phase < 10 ? phase : 20 - phase;
    return 100.0 + 0.5 * triangle + 0.25 * (index % 3);
}

std::vector<Bar> feed(int n) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double p = price_at(i);
        bars.push_back({p, p, p, p, 1.0, T + static_cast<std::int64_t>(i) * 60000});
    }
    return bars;
}

NativeRunSpec extension_spec(const char* key) {
    NativeRunSpec spec;
    spec.identity = {key, 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "TEST:R5N5";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 2.0;
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    return spec;
}

no::Request market(double units, const char* label) {
    no::Request request;
    request.intent = no::Transact{units};
    request.label = label;
    return request;
}

void round_trip(Host& host) {
    switch (host.calculations - 1) {
    case 10: host.submit(market(2.0, "enter-long")); break;
    case 30: host.submit(market(-2.0, "exit-long")); break;
    default: break;
    }
}

void run_feed(Host& host, const NativeRunSpec& spec, const std::vector<Bar>& bars) {
    REQUIRE(host.configure_native(spec).status == NativeSetupStatus::Applied);
    host.run(bars.data(), static_cast<int>(bars.size()));
}

std::vector<std::uint64_t> recorded_rows(const BacktestEngine& engine) {
    ReportC report{};
    engine.fill_report(&report);
    std::vector<std::uint64_t> rows;
    if (report.broker_state_hash_len > 0) {
        rows.assign(report.broker_state_hash,
                    report.broker_state_hash + report.broker_state_hash_len);
    }
    BacktestEngine::free_report(&report);
    return rows;
}

// ── Hosts ───────────────────────────────────────────────────────────────
// Test-only reach for the generic projection: it takes the execution hash as
// an argument, so two hosts are compared on their broker state and their own
// extension alone.
struct PlainHost : Host {
    std::uint64_t broker_state_from(std::uint64_t execution_hash) const {
        return broker_state_hash_from_execution_hash(execution_hash);
    }
};

// A host with durable state of its own — the regime its rule is in — folded
// under its own domain tag.
struct RegimeHost : PlainHost {
    std::int64_t regime = 0;
    mutable int folds = 0;
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        ++folds;
        sink.s("regime-host/v1");
        sink.i(regime);
    }
};

// Spells out what a host without an extension folds.
struct MarkerHost : PlainHost {
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        sink.s("source:none");
    }
};

// Keeps the kernel default and appends to it.
struct AppendingHost : PlainHost {
    std::int64_t regime = 0;
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        BacktestEngine::hash_host_extension(sink);
        sink.i(regime);
    }
};

// A host written against the deprecated spelling of the same seam.
struct DeprecatedSpellingHost : PlainHost {
    std::int64_t regime = 0;
    void hash_source_extension(BrokerStateHashSink& sink) const override {
        sink.s("regime-host/v1");
        sink.i(regime);
    }
};

// Both spellings overridden: the kernel folds the generic one.
struct BothSpellingsHost : RegimeHost {
    void hash_source_extension(BrokerStateHashSink& sink) const override {
        sink.s("never-folded");
    }
};

// ── Scenarios ───────────────────────────────────────────────────────────

// 1. Nothing moved for a host that overrides nothing: its broker state folds
//    to the values a clean main build produced, before and after a run, and
//    the generic default is exactly the established "no extension" marker.
void default_fold_is_unchanged() {
    PlainHost fresh;
    CHECK(fresh.broker_state_from(kProbeExecutionHash) == kMainFreshBrokerState);

    MarkerHost marker;
    CHECK(marker.broker_state_from(kProbeExecutionHash)
          == fresh.broker_state_from(kProbeExecutionHash));

    PlainHost ran;
    ran.calculation = round_trip;
    run_feed(ran, extension_spec("n5-hash-extension"), feed(60));
    completed(ran);
    CHECK(ran.rows().size() == 1);
    CHECK(ran.broker_state_from(kProbeExecutionHash) == kMainRoundTripBrokerState);
    CHECK(ran.broker_state_from(kProbeExecutionHash)
          != fresh.broker_state_from(kProbeExecutionHash));
}

// 2. A native host extends the fold through the generic hook: its state is
//    part of the hash, moves it, replays, and is folded exactly once per hash.
void a_native_host_extends_the_fold() {
    PlainHost plain;
    RegimeHost host;
    CHECK(host.broker_state_from(kProbeExecutionHash)
          != plain.broker_state_from(kProbeExecutionHash));

    const std::uint64_t calm = host.broker_state_from(kProbeExecutionHash);
    host.regime = 1;
    const std::uint64_t trending = host.broker_state_from(kProbeExecutionHash);
    CHECK(trending != calm);
    host.regime = 0;
    CHECK(host.broker_state_from(kProbeExecutionHash) == calm);

    RegimeHost replay;
    replay.regime = 1;
    CHECK(replay.broker_state_from(kProbeExecutionHash) == trending);

    // The public scalar and the stream fingerprint are the same fold.
    RegimeHost observed;
    observed.folds = 0;
    const std::uint64_t scalar_calm = observed.broker_state_hash();
    CHECK(observed.folds == 1);
    const std::uint64_t stream_calm = observed.stream_state_hash();
    observed.regime = 7;
    CHECK(observed.broker_state_hash() != scalar_calm);
    CHECK(observed.stream_state_hash() != stream_calm);
    observed.regime = 0;
    CHECK(observed.broker_state_hash() == scalar_calm);
    CHECK(observed.stream_state_hash() == stream_calm);

    // Appending to the kernel default is the other spelling of "extend": the
    // marker stays and the host's state follows it.
    AppendingHost appended;
    const std::uint64_t appended_calm = appended.broker_state_from(kProbeExecutionHash);
    CHECK(appended_calm != plain.broker_state_from(kProbeExecutionHash));
    appended.regime = 3;
    CHECK(appended.broker_state_from(kProbeExecutionHash) != appended_calm);
}

// 3. The rows a KernelRecorded run records are that same fold, so they carry
//    the extension: two hosts trading identically record equal rows until
//    their own state diverges inside calculation 20, and different rows from
//    that bar on. The extension is reporting input only — the trades and the
//    execution continuation of the two runs are the same.
void recorded_rows_carry_the_extension() {
    const auto spec = extension_spec("n5-hash-extension");

    RegimeHost steady;
    steady.set_broker_state_hash_recording(true);
    steady.calculation = round_trip;
    run_feed(steady, spec, feed(60));
    completed(steady);

    RegimeHost switching;
    switching.set_broker_state_hash_recording(true);
    switching.calculation = [](Host& host) {
        round_trip(host);
        if (host.calculations - 1 == 20) static_cast<RegimeHost&>(host).regime = 1;
    };
    run_feed(switching, spec, feed(60));
    completed(switching);

    const auto steady_rows = recorded_rows(steady);
    const auto switching_rows = recorded_rows(switching);
    REQUIRE(steady_rows.size() == 60);
    REQUIRE(switching_rows.size() == 60);
    CHECK(static_cast<std::int64_t>(steady_rows.size()) == steady.script_bars_processed());
    for (std::size_t i = 0; i < 20; ++i) CHECK(steady_rows[i] == switching_rows[i]);
    for (std::size_t i = 20; i < 60; ++i) CHECK(steady_rows[i] != switching_rows[i]);

    CHECK(steady.native_continuation_hash() == switching.native_continuation_hash());
    REQUIRE(steady.rows().size() == switching.rows().size());
    for (std::size_t i = 0; i < steady.rows().size(); ++i) {
        CHECK(steady.rows()[i].pnl == switching.rows()[i].pnl);
        CHECK(steady.rows()[i].exit_time == switching.rows()[i].exit_time);
    }

    // And every row differs from the plain host's: the fold is in all of them.
    PlainHost plain;
    plain.set_broker_state_hash_recording(true);
    plain.calculation = round_trip;
    run_feed(plain, spec, feed(60));
    completed(plain);
    const auto plain_rows = recorded_rows(plain);
    REQUIRE(plain_rows.size() == 60);
    for (std::size_t i = 0; i < 60; ++i) CHECK(plain_rows[i] != steady_rows[i]);
}

// 4. The deprecated spelling keeps folding: a host still written against it
//    hashes exactly like the same host written against the generic hook. When
//    both are overridden the kernel folds the generic one only.
void deprecated_spelling_still_folds() {
    DeprecatedSpellingHost legacy;
    RegimeHost generic;
    CHECK(legacy.broker_state_from(kProbeExecutionHash)
          == generic.broker_state_from(kProbeExecutionHash));
    legacy.regime = 4;
    generic.regime = 4;
    CHECK(legacy.broker_state_from(kProbeExecutionHash)
          == generic.broker_state_from(kProbeExecutionHash));
    CHECK(legacy.broker_state_hash() == generic.broker_state_hash());

    BothSpellingsHost both;
    both.regime = 4;
    CHECK(both.broker_state_from(kProbeExecutionHash)
          == generic.broker_state_from(kProbeExecutionHash));
}

}  // namespace

int main() {
    test("default_fold_is_unchanged", default_fold_is_unchanged);
    test("a_native_host_extends_the_fold", a_native_host_extends_the_fold);
    test("recorded_rows_carry_the_extension", recorded_rows_carry_the_extension);
    test("deprecated_spelling_still_folds", deprecated_spelling_still_folds);
    std::printf("test_native_host_hash_extension: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
