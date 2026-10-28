// The lab compiles this unit against the previous tree first: the missing
// internal live_report.hpp is the expected first diagnostic.
#include "../src/live_report.hpp"
#include "../src/native_execution_consumer.hpp"
#include "../runner/report.hpp"
#include "live_report_document_oracle.hpp"
#include "../runner/store.hpp"
#include "native_report_truth_fixture.hpp"

#include <cassert>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
using namespace pineforge::live;
using namespace r4_test;

// Independent consumer: deliberately does not use apply_report_delta.
void merge(Json& into, const Json& fields) {
    if (fields.kind != Json::Kind::Object) { into = fields; return; }
    into.kind = Json::Kind::Object;
    for (const auto& item : fields.members) merge(into.members[item.first], item.second);
}
void apply(Json& into, const Json& delta) {
    merge(into, delta.at("fields"));
    for (const auto* name : {"ratios_as_of_input", "ratio_context"})
        if (const auto* value = delta.find(name)) into.members[name] = *value;
    for (const auto& item : delta.at("arrays").members) {
        auto& owner = item.first == "closed_trades" ? into : into.members["report"];
        auto& values = owner.members[item.first];
        const auto offset = item.second.at("offset").integer<std::size_t>();
        assert(offset <= values.items.size());
        values.kind = Json::Kind::Array;
        values.items.resize(offset);
        for (const auto& row : item.second.at("items").items) values.items.push_back(row);
    }
}

std::vector<Bar> tape(int count) {
    std::uint64_t seed = 0x243f6a8885a308d3ULL;
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        const double p = 95.003 + static_cast<double>(seed % 2101) * 0.0137;
        // Daily observations cross several civil months, including DST.
        bars.push_back({p, p + 0.31, p - 0.23, p + 0.019, 3.0,
                        1706659200000LL + static_cast<std::int64_t>(i) * 86400000LL});
    }
    return bars;
}

void rule(Host& host) {
    const int i = host.calculations - 1;
    // An extended quiet stretch, partial exits, both directions and an open
    // terminal lot. Every request is executed by the real native stream.
    if (i >= 64 && i < 93) return;
    if (i % 18 == 2) put(host, tx((i / 18) % 2 ? -2.0 : 2.0));
    if (i % 18 == 4 && host.physical_position().signed_units != 0.0) put(host, reduce(0.75));
    if (i % 18 == 8 && host.physical_position().signed_units != 0.0) put(host, flat());
}

struct ReportHost : Host {
    void clear_report_mark() { range_end_trades_.clear(); }
    void mark_for_report() {
        range_end_trades_.clear();
        if (position_side_ == PositionSide::FLAT) return;
        // The bare host normally marks at stream_end. This fixture owns a
        // snapshot mark at every input, through the same production row
        // builder, so the source-free test also exercises real open-lot rows.
        NativeExecutionConsumer::bound(*this).append_open_position_report_rows(
            *this, bar_fill_price(current_bar_.close),
            last_equity_point() ? last_equity_point()->time_ms : current_bar_.timestamp,
            bar_index_, active_account_currency_fx());
    }
};

struct BatchReport {
    ReportC value{};
    explicit BatchReport(const BacktestEngine& engine) { engine.fill_report(&value); }
    ~BatchReport() { BacktestEngine::free_report(&value); }
    const pf_report_t& c() const { return *reinterpret_cast<const pf_report_t*>(&value); }
};

Json ratios(const Json& document) {
    const auto& equity = document.at("report").at("metrics").at("equity");
    Json result = Json::object({});
    for (const auto* key : {"sharpe_monthly", "sharpe_bar", "sortino_bar", "sortino_monthly"})
        result.members[key] = equity.at(key);
    return result;
}
void set_ratios(Json& document, const Json& values) {
    for (const auto& item : values.members)
        document.members["report"].members["metrics"].members["equity"].members[item.first] = item.second;
}

struct Result {
    report::LiveReportWork work;
    std::uint64_t batch_array_rows_returned = 0;
};

struct ExportLedger {
    std::string directory, path;
    std::unique_ptr<Ledger> ledger;
    explicit ExportLedger(bool enabled) {
        if (!enabled) return;
        directory = (std::filesystem::temp_directory_path() / "pineforge-ratio-export-XXXXXX").string();
        assert(mkdtemp(directory.data()));
        path = directory + "/report.sqlite3";
        ledger = std::make_unique<Ledger>(path, "seeded");
    }
    ~ExportLedger() {
        ledger.reset();
        if (!directory.empty()) std::filesystem::remove_all(directory);
    }
};

Result exercise(int inputs, std::uint64_t interval, const std::string& zone,
                bool final_in_update, bool exporting = false) {
    auto bars = tape(inputs + 3);
    auto config = l2_fixture::report_spec("live-report-fold");
    config.input_tf = "1D"; config.script_tf = "1D";
    config.report_policy = NativeReportPolicy::KernelRecorded;
    config.report_open_position_at_end = true;
    config.fee_value = 0.173;
    config.chart_timezone = zone;
    ReportHost actual, reference;
    actual.calculation = reference.calculation = rule;
    REQUIRE(actual.configure_native(config).status == NativeSetupStatus::Applied);
    REQUIRE(reference.configure_native(config).status == NativeSetupStatus::Applied);
    actual.set_broker_state_hash_recording(true);
    reference.set_broker_state_hash_recording(true);
    REQUIRE(actual.stream_begin(bars.data(), 3, "1D", "1D"));
    REQUIRE(reference.stream_begin(bars.data(), 3, "1D", "1D"));
    LiveReportAccumulator carried(interval);
    ReportDeltas deltas(true), batch_deltas(true);
    ExportLedger stored(exporting);
    Json applied, last_ratios;
    pf_equity_stats_t last_ratio_values{};
    std::uint64_t as_of = 0, batch_rows = 0;
    bool closed = false, opened = false, partial = false, short_trade = false, monthly_finite = false;
    for (int i = 0; i <= inputs; ++i) {
        if (i) {
            actual.clear_report_mark();
            reference.clear_report_mark();
            REQUIRE(actual.stream_push_bar(bars[static_cast<std::size_t>(i + 2)]));
            REQUIRE(reference.stream_push_bar(bars[static_cast<std::size_t>(i + 2)]));
        }
        actual.mark_for_report();
        reference.mark_for_report();
        const auto before_hash = actual.stream_state_hash();
        const auto before = carried.work();
        auto patch = carried.update(actual, static_cast<std::uint64_t>(i),
                                    final_in_update && i == inputs);
        assert(actual.stream_state_hash() == before_hash);
        assert(actual.stream_state_hash() == reference.stream_state_hash());
        const auto work = carried.work();
        assert(work.fallback_report_reads == 0);
        BatchReport batch(reference);
        // Batch reads neither charge nor reset the carried path's counters.
        assert(std::memcmp(&work, &carried.work(), sizeof(work)) == 0);
        batch_rows += batch.value.trades_len + batch.value.equity_curve_len;
        const auto hash = actual.stream_state_hash();
        const auto bytes = deltas.update(patch, "seeded", i, hash);
        const auto expected_delta = batch_deltas.update(batch.c(), "seeded", i, hash);
        auto expected = native_report_document(batch.c(), "seeded", i, hash);
        if (stored.ledger) {
            if (!i) stored.ledger->verify_report(0, bytes);
            else stored.ledger->commit_input(i - 1,
                Json::object({{"type", Json::string("time")},
                    {"ts", report_number(bars[static_cast<std::size_t>(i + 2)].timestamp)}}).dump(),
                hash, {}, bytes);
            auto exported_expected = expected;
            if (interval > 1) {
                exported_expected.members["ratios_as_of_input"] = report_number(i);
                exported_expected.members["ratio_context"] = Json::object({
                    {"chart_timezone", Json::string(zone)}});
            }
            assert(LedgerView(stored.path).report_json(i) == exported_expected.dump());
            assert(LedgerView(stored.path).report_json_full(i) == exported_expected.dump());
            // Export is read-only: the stored input payload remains the
            // interval delta, including its original ratio freshness marker.
            if (i) assert(stored.ledger->input(i - 1)->report_json == bytes);
        }
        const auto delta = parse_json(bytes);
        apply(applied, delta);
        const bool boundary = i == 0 || static_cast<std::uint64_t>(i) % interval == 0
            || (final_in_update && i == inputs);
        if (boundary) {
            last_ratios = ratios(expected);
            last_ratio_values = batch.value.metrics.equity;
            as_of = i;
        }
        auto expected_metrics = batch.value.metrics;
        auto& expected_equity = expected_metrics.equity;
        expected_equity.sharpe_monthly = last_ratio_values.sharpe_monthly;
        expected_equity.sortino_monthly = last_ratio_values.sortino_monthly;
        expected_equity.sharpe_bar = last_ratio_values.sharpe_bar;
        expected_equity.sortino_bar = last_ratio_values.sortino_bar;
        static_assert(sizeof(pf_metrics_t) == 768, "metric blocks have no padding");
        assert(std::memcmp(&patch.scalars.metrics, &expected_metrics, sizeof expected_metrics) == 0);
        if (interval == 1) {
            assert(bytes == expected_delta);
            assert(!delta.members.count("ratios_as_of_input"));
            assert(applied.dump() == expected.dump());
        } else {
            assert(delta.at("ratios_as_of_input").integer<std::uint64_t>() == as_of);
            assert(ratios(applied).dump() == last_ratios.dump());
            set_ratios(expected, last_ratios);
            expected.members["ratios_as_of_input"] = report_number(as_of);
            expected.members["ratio_context"] = Json::object({
                {"chart_timezone", Json::string(zone)}});
            assert(applied.dump() == expected.dump());
        }
        assert(deltas.json() == applied.dump());
        if (i) {
            // One input appends one equity point and at most a small number of
            // executions. The previous provisional rows may be revisited.
            assert(work.non_ratio_rows() - before.non_ratio_rows() < 100);
        }
        closed |= batch.value.metrics.all.num_trades > 0;
        for (int row = 0; row < batch.value.trades_len; ++row) {
            opened |= batch.value.trades[row].open_at_end != 0;
            partial |= !batch.value.trades[row].open_at_end && batch.value.trades[row].qty == 0.75;
            short_trade |= !batch.value.trades[row].is_long;
        }
        monthly_finite |= std::isfinite(batch.value.metrics.equity.sharpe_monthly);
    }
    assert(closed && opened && partial && short_trade);
    if (inputs >= 130) assert(monthly_finite);
    // An EOF discovered after the input update must still refresh the final
    // cumulative result, with no duplicate replay at an existing boundary.
    const auto final_patch = carried.update(actual, inputs, true);
    const auto final_delta = parse_json(deltas.update(final_patch, "seeded", inputs,
                                                     actual.stream_state_hash()));
    apply(applied, final_delta);
    BatchReport final_batch(reference);
    auto expected = native_report_document(final_batch.c(), "seeded", inputs,
                                           actual.stream_state_hash());
    if (interval > 1) {
        expected.members["ratios_as_of_input"] = report_number(inputs);
        expected.members["ratio_context"] = Json::object({
            {"chart_timezone", Json::string(zone)}});
    }
    assert(applied.dump() == expected.dump());
    assert(deltas.json() == expected.dump());
    const auto input_count = static_cast<std::uint64_t>(inputs);
    assert(carried.work().ratio_recomputations
        == input_count / interval + (input_count % interval != 0));
    assert(carried.work().warmup_ratio_recomputations == 1);
    return {carried.work(), batch_rows};
}

void exactness() {
    for (const auto* zone : {"UTC", "America/New_York"}) {
        exercise(131, 1, zone, true);
        exercise(131, 7, zone, true);
        exercise(131, 7, zone, false);
        exercise(133, 7, zone, true);
        exercise(31, 500, zone, false);
        exercise(31, std::numeric_limits<std::uint64_t>::max(), zone, false);
    }
    bool refused = false;
    try { LiveReportAccumulator invalid(0); }
    catch (const std::invalid_argument&) { refused = true; }
    assert(refused);
}

struct TailHost : Host {
    void name(const std::string& value) { intern_trace_name(value); }
    void replace_held(double equity, double open_profit) {
        assert(!equity_curve_.empty());
        equity_curve_.back().equity = equity;
        equity_curve_.back().open_profit = open_profit;
    }
    void present_report(ReportC* out) const override {
        if (!out->equity_curve_len) return;
        auto& tail = out->equity_curve[out->equity_curve_len - 1];
        tail.equity -= 0.137;
        tail.open_profit = 0.0;
        // Presentation can put the provisional observation in another civil
        // month. It must not enter the next update's immutable prefix.
        tail.time_ms += 32LL * 86400000LL;
    }
};

void replacement_tails() {
    const auto bars = tape(12);
    auto config = l2_fixture::report_spec("live-report-replacements");
    config.input_tf = config.script_tf = "1D";
    config.report_policy = NativeReportPolicy::KernelRecorded;
    TailHost actual, reference;
    REQUIRE(actual.configure_native(config).status == NativeSetupStatus::Applied);
    REQUIRE(reference.configure_native(config).status == NativeSetupStatus::Applied);
    REQUIRE(actual.stream_begin(bars.data(), 3, "1D", "1D"));
    REQUIRE(reference.stream_begin(bars.data(), 3, "1D", "1D"));
    LiveReportAccumulator carried;
    ReportDeltas deltas(true), expected_deltas(true);
    int next_bar = 3;
    for (std::uint64_t i = 0; i < 16; ++i) {
        if (i == 0 || i == 4) {
            actual.name(std::to_string(i));
            reference.name(std::to_string(i));
        }
        // Alternate a real appended observation with an in-place raw-tail
        // replacement and no new point. Repeated reads themselves also stay exact.
        if (i % 2 && next_bar < static_cast<int>(bars.size())) {
            REQUIRE(actual.stream_push_bar(bars[static_cast<std::size_t>(next_bar)]));
            REQUIRE(reference.stream_push_bar(bars[static_cast<std::size_t>(next_bar++)]));
        }
        if (i && i % 2 == 0) {
            const auto value = 10000.0 + static_cast<double>(i % 7) * 1.173;
            actual.replace_held(value, 0.31 * static_cast<double>(i));
            reference.replace_held(value, 0.31 * static_cast<double>(i));
        }
        const auto patch = carried.update(actual, i);
        BatchReport batch(reference);
        const auto hash = actual.stream_state_hash();
        assert(deltas.update(patch, "replaced", i, hash)
            == expected_deltas.update(batch.c(), "replaced", i, hash));
        assert(deltas.json() == native_report_document(batch.c(), "replaced", i, hash).dump());
    }
    assert(carried.work().fallback_report_reads == 0);
    // HostRecorded has no equity observations: all zero-length patches and
    // batch NaN rules still need to serialize exactly.
    Host empty;
    config.report_policy = NativeReportPolicy::HostRecorded;
    REQUIRE(empty.configure_native(config).status == NativeSetupStatus::Applied);
    REQUIRE(empty.stream_begin(bars.data(), 3, "1D", "1D"));
    LiveReportAccumulator empty_carried;
    ReportDeltas empty_deltas(true);
    const auto patch = empty_carried.update(empty, 0);
    BatchReport batch(empty);
    empty_deltas.update(patch, "empty", 0, empty.stream_state_hash());
    assert(empty_deltas.json()
        == native_report_document(batch.c(), "empty", 0, empty.stream_state_hash()).dump());
}

void growth() {
    for (const auto interval : {std::uint64_t{1}, std::uint64_t{7}}) {
        const auto small = exercise(32, interval, "UTC", true);
        const auto large = exercise(320, interval, "UTC", true);
        assert(small.work.non_ratio_rows() > 0);
        assert(large.work.non_ratio_rows() < 12 * small.work.non_ratio_rows());
        // This is the independent batch output's array cardinality, not an
        // operation counter. Batch counts are also covered by byte equality.
        assert(large.batch_array_rows_returned > 20 * small.batch_array_rows_returned);
    }
}
}

int main(int argc, char** argv) {
    assert(argc == 2);
    if (std::string(argv[1]) == "equivalence") { exactness(); replacement_tails(); }
    else if (std::string(argv[1]) == "export") {
        exercise(131, 7, "America/New_York", false, true);
        exercise(31, 1, "UTC", true, true);
    }
    else { assert(std::string(argv[1]) == "growth"); growth(); }
    std::cout << "live report accumulator: " << argv[1] << '\n';
    return r4_test::failures ? 1 : 0;
}
