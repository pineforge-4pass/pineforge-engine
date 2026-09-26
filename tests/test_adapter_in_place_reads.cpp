// R5 lane D2-C: the Pine layer's in-place reads answer what the host's
// accessors answer, and the running spec's fact the host keeps once per run
// answers what it would compute.
//
// The source layer reads the run's lifecycle, spec and phase, the callback's
// point, the book and the bound consumer through src/source/pine_host_reads.hpp
// instead of native_state(), current_execution_point(), physical_position()
// and execution_consumer(), and reads the kernel's aggregation answer
// (native_aggregates_input_bars, since R5 lane H-THIN; the adapter's own
// literal predicate before) once per run, kept in the adapter's host cache
// (pd::PineRunCache, taken at on_native_run_begin). For every D2-C scenario
// (tests/pine_d2c_scenarios_fixture.hpp: magnifier off/on, aggregated charts,
// calc_on_order_fills re-entries, warm-up switches, kernel-routed and
// host-driven request.security sites, the auxiliary lower-timeframe array,
// the native daily partition, the suppressed probe tail, a stream):
//
//   1. The host compares, at every script bar it publishes, in every
//      request.security evaluation and at the run's script preparation, each
//      in-place read against its accessor -- field for field, the point by
//      its digest -- and the kept fact against the host's
//      native_aggregates_input_bars(); and again before the run
//      (Unconfigured) and after it (Completed).
//   2. The scenario runs with the consumer's host cache (shipped) and without
//      it (set_host_cache(false): nothing kept, every read of the fact
//      computes it, the PERF-P7 lookups walk): the transcripts -- every read
//      above, every trade, the broker-state hash and the continuation -- are
//      identical, and the kept fact answered the host's per-bar reads in the
//      first run and never in the second.
//
// Fail-before: at the lane's base src/source/pine_host_reads.hpp does not
// exist, so this TU does not compile there (the lane report records the
// first diagnostic).
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/native_execution_consumer.hpp"
#include "../src/source/pine_host_reads.hpp"
#include "pine_d2c_scenarios_fixture.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
using namespace d2c_scenarios;
namespace pd = pineforge::source::detail;

int failures = 0;
long checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

std::uint64_t fold(std::uint64_t h, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (8 * i)) & 0xFFu;
        h *= 1099511628211ull;
    }
    return h;
}

std::uint64_t point_digest(const NativeCurrentPointView& point) {
    const NativeDecisionContext& d = point.decision;
    std::uint64_t h = 1469598103934665603ull;
    h = fold(h, d.coordinate.ordinal);
    h = fold(h, static_cast<std::uint64_t>(d.coordinate.interval_index));
    h = fold(h, static_cast<std::uint64_t>(d.coordinate.input_interval_index));
    h = fold(h, static_cast<std::uint64_t>(d.coordinate.effective_time_ms));
    h = fold(h, static_cast<std::uint64_t>(d.coordinate.provenance));
    h = fold(h, static_cast<std::uint64_t>(d.coordinate.path_phase));
    h = fold(h, static_cast<std::uint64_t>(d.decision_floor_ms));
    h = fold(h, static_cast<std::uint64_t>(d.sub_index));
    h = fold(h, static_cast<std::uint64_t>(d.sub_count));
    h = fold(h, (d.in_session ? 1u : 0u) | (d.opens_session_day ? 2u : 0u)
                    | (d.closes_session_day ? 4u : 0u) | (d.is_terminal_sub_bar ? 8u : 0u));
    h = fold(h, static_cast<std::uint64_t>(d.sub_bar_open_ms));
    h = fold(h, static_cast<std::uint64_t>(d.script_bar_open_ms));
    h = fold(h, static_cast<std::uint64_t>(point.quote_kind));
    h = fold(h, point.quote_origin_ordinal);
    std::uint64_t price = 0;
    std::memcpy(&price, &point.price, sizeof price);
    return fold(h, price);
}

class InPlaceHost final : public source::PineStrategyHost {
public:
    explicit InPlaceHost(const Scenario& scenario) : scenario_(scenario) {
        attach_pine_execution_adapter();
        configure_pine_strategy(scenario.config);
        d2c_scenarios::configure(*this, scenario);
    }

    std::vector<std::string> lines;
    long compared = 0;
    long kept_compared = 0;

    NativeExecutionConsumer& consumer() { return pd::run_consumer(*this); }

    // The adapter's kept facts, when the consumer holds them.
    const pd::PineRunCache* kept() const {
        NativeHostCache* const cache = pd::run_consumer(*this).host_cache();
        if (cache == nullptr || cache->kind() != &pd::kPineRunCacheKind) return nullptr;
        const auto* facts = static_cast<const pd::PineRunCache*>(cache);
        return facts->owner == &adapter_ ? facts : nullptr;
    }

    // Every in-place read against its accessor; answers a transcript fragment.
    std::string compare() const {
        const NativeExecutionConsumer& consumer = pd::run_consumer(*this);
        CHECK(&consumer == &as_native_consumer(execution_consumer()));
        const NativeStateView view = native_state();
        const pd::RunState state = pd::run_state(*this);
        CHECK(state.kind == view.kind);
        CHECK(state.spec == view.spec);
        CHECK(state.phase == view.phase);
        CHECK(pd::run_kind(*this) == view.kind);
        CHECK(pd::run_spec(*this) == view.spec);
        CHECK(pd::run_phase(*this) == view.phase);
        const NativePhysicalPosition position = physical_position();
        const NativePhysicalPosition in_place = pd::run_position(*this);
        CHECK(bits(position.signed_units) == bits(in_place.signed_units));
        CHECK(bits(position.average_price) == bits(in_place.average_price));
        CHECK(position.lot_count == in_place.lot_count);
        const auto point = current_execution_point();
        const NativeCurrentPointView* point_in_place = pd::callback_point(*this);
        CHECK(point.has_value() == (point_in_place != nullptr));
        std::uint64_t digest = 0;
        if (point && point_in_place) {
            digest = point_digest(*point);
            CHECK(digest == point_digest(*point_in_place));
        }
        const bool aggregates = native_aggregates_input_bars();
        if (const pd::PineRunCache* facts = kept();
            facts != nullptr && view.kind == NativeLifecycleKind::Running) {
            CHECK(facts->aggregates_input_bars.has_value());
            if (facts->aggregates_input_bars) CHECK(*facts->aggregates_input_bars == aggregates);
            ++const_cast<InPlaceHost*>(this)->kept_compared;
        }
        ++const_cast<InPlaceHost*>(this)->compared;
        return " kind=" + std::to_string(static_cast<int>(view.kind))
            + " phase=" + std::to_string(static_cast<int>(view.phase))
            + " pos=" + bits(position.signed_units) + "/" + bits(position.average_price)
            + "/" + std::to_string(position.lot_count) + " point=" + std::to_string(digest)
            + " agg=" + std::to_string(aggregates);
    }

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        for (const Site& site : scenario_.sites) {
            if (site.lower_array) register_security_lower_tf_eval(site.id, site.tf, input_tf_);
            else register_security_eval(site.id, site.tf, input_tf_, site.lookahead, false);
        }
    }

    void prepare_script_run(const Bar* bars, int n, bool precalculation) override {
        PineStrategyHost::prepare_script_run(bars, n, precalculation);
        lines.push_back("prepare" + compare());
    }

    void on_source_bar(const Bar& bar) override {
        const int index = pine_bar_index();
        lines.push_back("bar " + std::to_string(index) + " " + std::to_string(current_bar_.timestamp)
                        + (history_advances_new_bar() ? " new" : " again") + compare());
        if (history_advances_new_bar()) place_orders(*this, index, bar);
    }

    void evaluate_security(int sec_id, const Bar& bar, bool is_complete) override {
        lines.push_back("sec " + std::to_string(sec_id) + " " + std::to_string(bar.timestamp)
                        + (is_complete ? " complete" : " partial") + compare());
    }

    void clear_security(int) override {}

private:
    const Scenario& scenario_;
};

struct Result {
    std::vector<std::string> lines;
    std::uint64_t fact_answers = 0;
    bool kept = false;
    long compared = 0;
    long kept_compared = 0;
    std::string error;
};

Result run(const Scenario& scenario, bool host_cache) {
    InPlaceHost host(scenario);
    host.consumer().set_host_cache(host_cache);
    host.lines.push_back("constructed" + host.compare());
    CHECK(drive(host, scenario));
    Result result;
    result.error = host.last_error();
    host.lines.push_back("finished" + host.compare());
    if (const pd::PineRunCache* facts = host.kept()) {
        result.kept = true;
        result.fact_answers = facts->fact_answers;
    }
    result.compared = host.compared;
    result.kept_compared = host.kept_compared;
    result.lines = std::move(host.lines);
    trade_lines(host, result.lines);
    result.lines.push_back("broker " + std::to_string(host.broker_state_hash()));
    result.lines.push_back("continuation " + std::to_string(host.consumer().continuation_hash()));
    return result;
}

void kept_and_computed_reads_agree() {
    long lines = 0;
    long compared = 0;
    std::uint64_t answers = 0;
    int runs = 0;
    for (const Scenario& scenario : scenarios()) {
        const Result cached = run(scenario, true);
        const Result computed = run(scenario, false);
        CHECK(cached.error.empty());
        CHECK(computed.error.empty());
        CHECK(cached.lines.size() > 50);
        CHECK(cached.lines.size() == computed.lines.size());
        const std::size_t common = std::min(cached.lines.size(), computed.lines.size());
        for (std::size_t i = 0; i < common; ++i) {
            if (cached.lines[i] != computed.lines[i]) {
                std::fprintf(stderr, "  %s: first divergence at line %zu\n    cached   %s\n"
                             "    computed %s\n", scenario.name.c_str(), i,
                             cached.lines[i].c_str(), computed.lines[i].c_str());
                ++failures;
                break;
            }
        }
        // The kept fact answered the host's own per-bar reads; without a
        // host cache nothing was kept.
        CHECK(cached.kept);
        CHECK(cached.fact_answers > 0);
        CHECK(cached.kept_compared > 0);
        CHECK(!computed.kept);
        CHECK(computed.fact_answers == 0);
        std::printf("  %-20s %5zu lines, %6ld reads compared, %4" PRIu64 " kept-fact answers\n",
                    scenario.name.c_str(), cached.lines.size(), cached.compared,
                    cached.fact_answers);
        lines += static_cast<long>(cached.lines.size());
        compared += cached.compared + computed.compared;
        answers += cached.fact_answers;
        ++runs;
    }
    std::printf("pine hosts: %d scenarios, %ld transcript lines identical with and without the "
                "host cache, %ld reads compared, %" PRIu64 " kept-fact answers\n",
                runs, lines, compared, answers);
}

}  // namespace

int main() {
    kept_and_computed_reads_agree();
    if (failures != 0) {
        std::fprintf(stderr, "test_adapter_in_place_reads: %d failure(s) in %ld checks\n",
                     failures, checks);
        return 1;
    }
    std::printf("test_adapter_in_place_reads: ok (%ld checks)\n", checks);
    return 0;
}
