// R5 lane L12b (design 2.ii row m): the request.security evaluator semantics
// that left BacktestEngine::SecurityEvalState live in a per-sec_id side table
// on the source host, source::PineSecurityEvalState, folded by the source hash
// extension under its own domain and only when a site is registered.
//
// Literal state construction only: no run, no stream, no strategy command.
//
//   1. a host with no request.security site folds nothing of the table;
//   2. registering a site enters the fold, and every field of the table moves
//      the broker hash (the static coverage checker enumerates the same
//      fields; this is its executed twin, so a fold behind a dead branch
//      cannot pass);
//   3. the fold is keyed by sec_id, not by registration order;
//   4. generated configure_security_evaluators() opens with
//      security_eval_states_.clear(): the first registration into the empty
//      registry starts the table over, so a site's semantics live exactly as
//      long as its kernel evaluator state;
//   5. the kernel evaluator state a site registers is the generic one -- the
//      source host's registration puts no aggregator under a lower-timeframe
//      site and an ordinary one under a coarser site.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                \
        }                                                                      \
    } while (0)

class Probe final : public source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        // A hash-only fixture never reaches a source bar.
        std::abort();
    }
    void evaluate_security(int, const Bar&, bool) override { std::abort(); }

    void add_site(int sec_id, const std::string& tf, bool lookahead_on = false,
                  bool gaps_on = false, bool heikinashi = false) {
        register_security_eval(sec_id, tf, "15", lookahead_on, gaps_on, heikinashi);
    }
    void add_lower_tf_site(int sec_id, const std::string& tf) {
        register_security_lower_tf_eval(sec_id, tf, "15");
    }
    // What generated configure_security_evaluators() does first.
    void clear_registry() { security_eval_states_.clear(); }

    source::PineSecurityEvalState& site(int sec_id) { return pine_security_states_[sec_id]; }
    bool has_site(int sec_id) const { return pine_security_states_.count(sec_id) != 0; }
    std::size_t sites() const { return pine_security_states_.size(); }
    std::size_t evaluators() const { return security_eval_states_.size(); }
    bool evaluator_aggregates(std::size_t index) const {
        return security_eval_states_[index].aggregator.is_active();
    }
    int sub_bar_index(int sec_id) const { return security_lower_tf_sub_bar_index(sec_id); }
    bool slot_is_new(int sec_id) const { return security_series_slot_is_new(sec_id); }
    void set_sub_bar_count(std::size_t index, int count) {
        security_eval_states_[index].current_sub_bar_count = count;
    }
};

Bar literal_bar(std::int64_t ts, double close) {
    Bar bar{};
    bar.timestamp = ts;
    bar.open = close - 1.0;
    bar.high = close + 2.0;
    bar.low = close - 2.0;
    bar.close = close;
    bar.volume = 10.0;
    return bar;
}

struct Mutation {
    const char* name;
    void (*apply)(source::PineSecurityEvalState&);
};

// One row per field of source::PineSecurityEvalState and of its two nested
// records. scripts/check_broker_state_hash_coverage.py enumerates the same
// structs statically.
const Mutation kMutations[] = {
    {"lookahead_on", [](source::PineSecurityEvalState& s) { s.lookahead_on = true; }},
    {"gaps_on", [](source::PineSecurityEvalState& s) { s.gaps_on = true; }},
    {"publish_gate_tf_seconds",
     [](source::PineSecurityEvalState& s) { s.publish_gate_tf_seconds = 300; }},
    {"calling_close_completes_partial",
     [](source::PineSecurityEvalState& s) { s.calling_close_completes_partial = true; }},
    {"heikinashi", [](source::PineSecurityEvalState& s) { s.heikinashi = true; }},
    {"ha_prev_open", [](source::PineSecurityEvalState& s) { s.ha_prev_open = 101.5; }},
    {"ha_prev_close", [](source::PineSecurityEvalState& s) { s.ha_prev_close = 102.5; }},
    {"ha_seeded", [](source::PineSecurityEvalState& s) { s.ha_seeded = true; }},
    {"historical_projections",
     [](source::PineSecurityEvalState& s) {
         s.historical_projections.push_back(
             source::HistoricalSecurityProjection{literal_bar(1000, 100.0), 1000, true});
     }},
    {"lower_tf_requested",
     [](source::PineSecurityEvalState& s) { s.lower_tf_requested = true; }},
    {"lower_tf_emulation",
     [](source::PineSecurityEvalState& s) { s.lower_tf_emulation = true; }},
    {"lower_tf_ratio", [](source::PineSecurityEvalState& s) { s.lower_tf_ratio = 3; }},
    {"lower_tf_seconds", [](source::PineSecurityEvalState& s) { s.lower_tf_seconds = 300; }},
    {"lower_tf_array_requested",
     [](source::PineSecurityEvalState& s) { s.lower_tf_array_requested = true; }},
    {"lower_tf_sub_bar_index",
     [](source::PineSecurityEvalState& s) { s.lower_tf_sub_bar_index = 2; }},
    {"lower_tf_use_input",
     [](source::PineSecurityEvalState& s) { s.lower_tf_use_input = true; }},
    {"lower_tf_input_aggregation_ratio",
     [](source::PineSecurityEvalState& s) { s.lower_tf_input_aggregation_ratio = 5; }},
    {"lower_tf_input_buffer",
     [](source::PineSecurityEvalState& s) {
         s.lower_tf_input_buffer.push_back(literal_bar(2000, 50.0));
     }},
    {"calling_open_latches_first",
     [](source::PineSecurityEvalState& s) { s.calling_open_latches_first = true; }},
    {"first_bucket_published",
     [](source::PineSecurityEvalState& s) { s.first_bucket_published = true; }},
    {"slice_open_label", [](source::PineSecurityEvalState& s) { s.slice_open_label = 3000; }},
    {"last_published_label",
     [](source::PineSecurityEvalState& s) { s.last_published_label = 4000; }},
    {"deferred_aux",
     [](source::PineSecurityEvalState& s) {
         s.deferred_aux.push_back(source::DeferredAuxBar{literal_bar(5000, 75.0), 5060, true});
     }},
};

// Fields that only matter once a record exists: the projection in hand and
// one held-back auxiliary bar. Each row mutates a host that already carries
// the record, against a base that carries the same record unmutated.
const Mutation kRecordMutations[] = {
    {"historical_projection_cursor",
     [](source::PineSecurityEvalState& s) { s.historical_projection_cursor = 1; }},
    {"historical_projection_dispatched",
     [](source::PineSecurityEvalState& s) { s.historical_projection_dispatched = true; }},
    {"HistoricalSecurityProjection.bar",
     [](source::PineSecurityEvalState& s) { s.historical_projections[0].bar.close = 111.0; }},
    {"HistoricalSecurityProjection.first_child_ms",
     [](source::PineSecurityEvalState& s) { s.historical_projections[0].first_child_ms = 1060; }},
    {"HistoricalSecurityProjection.is_complete",
     [](source::PineSecurityEvalState& s) { s.historical_projections[0].is_complete = false; }},
    {"DeferredAuxBar.bar",
     [](source::PineSecurityEvalState& s) { s.deferred_aux[0].bar.close = 76.0; }},
    {"DeferredAuxBar.next_input_ms",
     [](source::PineSecurityEvalState& s) { s.deferred_aux[0].next_input_ms = 5120; }},
    {"DeferredAuxBar.calling_bar_complete",
     [](source::PineSecurityEvalState& s) { s.deferred_aux[0].calling_bar_complete = false; }},
};

void seed_records(source::PineSecurityEvalState& s) {
    s.historical_projections.push_back(
        source::HistoricalSecurityProjection{literal_bar(1000, 100.0), 1000, true});
    s.historical_projections.push_back(
        source::HistoricalSecurityProjection{literal_bar(1900, 105.0), 1900, true});
    s.deferred_aux.push_back(source::DeferredAuxBar{literal_bar(5000, 75.0), 5060, true});
}

}  // namespace

int main() {
    // 1. No site: two fresh hosts agree, and the table is empty.
    {
        const Probe a, b;
        CHECK(a.broker_state_hash() == b.broker_state_hash());
        CHECK(a.sites() == 0);
    }

    // 2. A registered site enters the fold; every field moves the hash.
    {
        const Probe none;
        Probe plain;
        plain.add_site(0, "60");
        CHECK(plain.sites() == 1);
        CHECK(plain.evaluators() == 1);
        CHECK(none.broker_state_hash() != plain.broker_state_hash());

        for (const Mutation& mutation : kMutations) {
            Probe base, changed;
            base.add_site(0, "60");
            changed.add_site(0, "60");
            CHECK(base.broker_state_hash() == changed.broker_state_hash());
            mutation.apply(changed.site(0));
            if (base.broker_state_hash() == changed.broker_state_hash()) {
                std::printf("FAIL PineSecurityEvalState.%s: broker hash unchanged\n",
                            mutation.name);
                ++g_failures;
            }
            ++g_checks;
        }
        for (const Mutation& mutation : kRecordMutations) {
            Probe base, changed;
            base.add_site(0, "60");
            changed.add_site(0, "60");
            seed_records(base.site(0));
            seed_records(changed.site(0));
            CHECK(base.broker_state_hash() == changed.broker_state_hash());
            mutation.apply(changed.site(0));
            if (base.broker_state_hash() == changed.broker_state_hash()) {
                std::printf("FAIL %s: broker hash unchanged\n", mutation.name);
                ++g_failures;
            }
            ++g_checks;
        }
        // The registration flags reach the table, and so the hash.
        Probe looked, gapped, candled;
        looked.add_site(0, "60", /*lookahead_on=*/true);
        gapped.add_site(0, "60", false, /*gaps_on=*/true);
        candled.add_site(0, "60", false, false, /*heikinashi=*/true);
        CHECK(looked.site(0).lookahead_on);
        CHECK(gapped.site(0).gaps_on);
        CHECK(candled.site(0).heikinashi);
        CHECK(looked.broker_state_hash() != plain.broker_state_hash());
        CHECK(gapped.broker_state_hash() != plain.broker_state_hash());
        CHECK(candled.broker_state_hash() != plain.broker_state_hash());
        CHECK(looked.broker_state_hash() != gapped.broker_state_hash());
    }

    // 3. Keyed by sec_id: registration order does not reach the fold, the
    //    sec_id a site's semantics sit under does.
    {
        Probe forward, backward, swapped;
        forward.add_site(0, "60", /*lookahead_on=*/true);
        forward.add_site(1, "240");
        backward.add_site(1, "240");
        backward.add_site(0, "60", /*lookahead_on=*/true);
        swapped.add_site(0, "60");
        swapped.add_site(1, "240", /*lookahead_on=*/true);
        CHECK(forward.broker_state_hash() == backward.broker_state_hash());
        CHECK(forward.broker_state_hash() != swapped.broker_state_hash());
    }

    // 4. The generated clear-then-register pattern starts the table over.
    {
        Probe host;
        host.add_site(0, "60", /*lookahead_on=*/true, /*gaps_on=*/true);
        host.add_site(3, "240", false, false, /*heikinashi=*/true);
        host.site(0).ha_seeded = true;
        CHECK(host.sites() == 2);
        host.clear_registry();
        host.add_site(0, "60");
        CHECK(host.evaluators() == 1);
        CHECK(host.sites() == 1);
        CHECK(!host.has_site(3));
        CHECK(!host.site(0).lookahead_on);
        CHECK(!host.site(0).gaps_on);
        CHECK(!host.site(0).ha_seeded);
        Probe fresh;
        fresh.add_site(0, "60");
        CHECK(host.broker_state_hash() == fresh.broker_state_hash());
        // Re-registering one sec_id without a clear resets that site alone.
        host.site(0).ha_prev_close = 9.0;
        host.add_site(0, "60", /*lookahead_on=*/true);
        CHECK(host.site(0).lookahead_on);
        CHECK(host.site(0).ha_prev_close == 0.0);
    }

    // 5. The kernel state underneath is the generic evaluator; the Pine
    //    predicates answer from the table.
    {
        Probe host;
        host.add_site(0, "60");            // coarser than the 15m input
        host.add_lower_tf_site(1, "5");    // finer: synthesized, no aggregator
        host.add_site(2, "15");            // same timeframe: passthrough
        CHECK(host.evaluators() == 3);
        CHECK(host.evaluator_aggregates(0));
        CHECK(!host.evaluator_aggregates(1));
        CHECK(!host.evaluator_aggregates(2));
        CHECK(host.site(1).lower_tf_array_requested);
        CHECK(host.site(1).lower_tf_emulation);
        CHECK(host.site(1).lower_tf_ratio == 3);
        CHECK(host.site(1).lower_tf_seconds == 300);
        CHECK(!host.site(0).lower_tf_requested);
        host.site(1).lower_tf_sub_bar_index = 2;
        CHECK(host.sub_bar_index(1) == 2);
        CHECK(host.sub_bar_index(0) == 0);
        CHECK(host.sub_bar_index(99) == 0);

        // security_series_slot_is_new: a lookahead_on site recomputes every
        // sub-bar after a bucket's first; a lookahead_off site never does.
        host.set_sub_bar_count(0, 3);
        CHECK(host.slot_is_new(0));
        host.site(0).lookahead_on = true;
        CHECK(!host.slot_is_new(0));
        host.set_sub_bar_count(0, 1);
        CHECK(host.slot_is_new(0));
        CHECK(host.slot_is_new(99));
    }

    std::printf("pine security state hash: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
