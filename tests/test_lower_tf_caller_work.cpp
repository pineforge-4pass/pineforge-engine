#include <pineforge/source/pine_strategy_host.hpp>

// The Debug counters of the request caller catalog are declared here.
#include "../src/source/pine_host_reads.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#ifdef PF_COUNT_DELIVERY_SCOPES
static std::uint64_t completed_scopes;
extern "C" void __real__ZN9pineforge6source24PineRequestDeliveryScopeD1Ev(
    pineforge::source::PineRequestDeliveryScope*) noexcept;
extern "C" void __wrap__ZN9pineforge6source24PineRequestDeliveryScopeD1Ev(
        pineforge::source::PineRequestDeliveryScope* scope) noexcept {
    ++completed_scopes;
    __real__ZN9pineforge6source24PineRequestDeliveryScopeD1Ev(scope);
}
#endif

namespace {

using namespace pineforge;

constexpr int chart_bars = 8;
constexpr std::int64_t origin = 1'704'067'200'000;
constexpr std::int64_t minute_ms = 60'000LL;

struct Probe final : source::PineStrategyHost {
    explicit Probe(const char* tf) : requested_tf(tf) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_type("crypto");
    }

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_lower_tf_eval(0, requested_tf, input_tf_);
    }

    void evaluate_security(int sec_id, const Bar&, bool publish) override {
        assert(sec_id == 0 && publish);
        const auto delivery = current_request_delivery(sec_id);
        assert(delivery.caller_valid && delivery.actual_complete && !delivery.replay);
        ++deliveries;
    }

    void on_source_bar(const Bar&) override { ++bodies; }

    const char* requested_tf;
    int deliveries = 0;
    int bodies = 0;
};

struct Work {
    std::uint64_t lookups;
    std::uint64_t insertions;
};

// A delivery callback may run another host on the same thread. Its caller
// catalog has a different lifetime; it must not evict this site's position.
struct InterleavedHost final : source::PineStrategyHost {
    static constexpr int slots = 12;
    static constexpr int charts = 7;
    static constexpr int site = 19;

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_lower_tf_eval(site, "3", input_tf_);
    }
    void on_source_bar(const Bar&) override { ++bodies; }
    void evaluate_security(int id, const Bar& bar, bool publish) override {
        const auto delivery = current_request_delivery(id);
        const int index = security_lower_tf_sub_bar_index(id);
        const double expected_close = 37.5 + calls / slots;
        if (id != site || !publish || !delivery.caller_valid || !delivery.actual_complete
            || delivery.requested_slot != calls || index != calls % slots || bar.close != expected_close) {
            std::fprintf(stderr, "NOT COMPARED: site %d/%d, publish %d/1, caller valid %d/1,"
                         " complete %d/1, slot %lld/%d, index %d/%d, close %a/%a\n",
                         id, site, publish, delivery.caller_valid, delivery.actual_complete,
                         static_cast<long long>(delivery.requested_slot), calls,
                         index, calls % slots, bar.close, expected_close);
            std::abort();
        }
        if (index != 0) searches += source::detail::request_caller_lookups_for_test - before;
        // An independent catalog starts a new hint generation on the base.
        // Its constructor inserts no rows and does no ordered search.
        { decltype(request_caller_ids_) other; }
        before = source::detail::request_caller_lookups_for_test;
        ++calls;
    }
    int calls = 0, bodies = 0;
    std::uint64_t searches = 0, before = 0;
};

void interleaved_catalog_work() {
    InterleavedHost host;
    host.set_syminfo_timezone("UTC");
    host.set_syminfo_session("24x7");
    host.set_syminfo_type("crypto");
    std::vector<Bar> bars;
    for (int i = 0; i < host.charts; ++i) {
        const double flat = 37.5 + i;
        bars.push_back({flat, flat, flat, flat, 84., (1440LL + 36 * i) * minute_ms});
    }
#ifdef PF_COUNT_DELIVERY_SCOPES
    completed_scopes = 0;
#endif
    host.run(bars.data(), host.charts, "36", "36");
    if (!host.last_error().empty() || host.calls != host.charts * host.slots
        || host.bodies != host.charts) {
        std::fprintf(stderr, "NOT COMPARED: calls %d/%d, chart bars %d/%d, error '%s'\n",
                     host.calls, host.charts * host.slots, host.bodies, host.charts,
                     host.last_error().c_str());
        std::abort();
    }
    bool ok = host.searches == 0;
    if (!ok)
        std::fprintf(stderr, "FAIL: later slots searched the caller catalog %llu times, expected 0\n",
                     static_cast<unsigned long long>(host.searches));
#ifdef PF_COUNT_DELIVERY_SCOPES
    std::fprintf(stderr, "intrabar work: %d chart bars, %d callbacks, %llu later-slot searches, %llu scopes\n",
                 host.bodies, host.calls, static_cast<unsigned long long>(host.searches),
                 static_cast<unsigned long long>(completed_scopes));
    if (completed_scopes != host.charts) {
        std::fprintf(stderr, "FAIL: completed delivery scopes %llu, expected %d for %d callbacks\n",
                     static_cast<unsigned long long>(completed_scopes), host.charts, host.calls);
        ok = false;
    }
#endif
    assert(ok);
}

// One run of `charts` chart bars of 240 minutes, fed as input bars of
// `input_minutes`, with one array request on `requested_tf`.
Work measure(const char* requested_tf, int sub_bars, int charts = chart_bars,
             const char* input_tf = "240", int input_minutes = 240) {
    const int inputs = charts * (240 / input_minutes);
    std::vector<Bar> bars(static_cast<std::size_t>(inputs));
    for (int i = 0; i < inputs; ++i) {
        const double price = 100.0 + i;
        bars[static_cast<std::size_t>(i)] = {
            price, price + 2, price - 1, price + 1, 240.0,
            origin + i * input_minutes * minute_ms};
    }
    Probe strategy(requested_tf);
    source::detail::request_caller_lookups_for_test = 0;
    source::detail::request_caller_insertions_for_test = 0;
    strategy.run(bars.data(), inputs, input_tf, "240");
    assert(strategy.last_error().empty());
    assert(strategy.bodies == charts);
    assert(strategy.deliveries == charts * sub_bars);
    return {source::detail::request_caller_lookups_for_test,
            source::detail::request_caller_insertions_for_test};
}

void print_work(const char* what, const Work& coarse, const Work& fine) {
    std::fprintf(stderr, "%s: caller lookups/insertions %llu/%llu against %llu/%llu\n",
                 what, static_cast<unsigned long long>(coarse.lookups),
                 static_cast<unsigned long long>(coarse.insertions),
                 static_cast<unsigned long long>(fine.lookups),
                 static_cast<unsigned long long>(fine.insertions));
}

// The controls live in this namespace so that its names are found before any
// name of the engine's namespaces.
void lower_tf_caller_work_controls() {
    // Synthesized sub-bars: the work must not grow with their number.
    const Work coarse = measure("15", 16);
    const Work fine = measure("1", 240);
    if (fine.lookups != coarse.lookups || fine.insertions != coarse.insertions)
        print_work("15m against 1m", coarse, fine);
    assert(fine.lookups == coarse.lookups && fine.insertions == coarse.insertions);
    assert(fine.lookups > 0 && fine.lookups <= 4 * chart_bars);
    // expectation corrected: insertions <= lookups -> insertions <= 3 per chart
    // bar, because the lookups now count ordered searches only and a row that
    // is placed from the last position needs none. The three rows are the
    // chart caller, the input caller and the first slot of the group.
    assert(fine.insertions > 0 && fine.insertions <= 3 * chart_bars);

    // Chart bars: every ordered search belongs to the first one. Twice the
    // chart bars add their rows and not one search.
    const Work longer = measure("1", 240, 2 * chart_bars);
    if (longer.lookups != fine.lookups || longer.insertions != 2 * fine.insertions)
        print_work("8 against 16 chart bars", fine, longer);
    assert(longer.lookups == fine.lookups);
    assert(longer.insertions == 2 * fine.insertions);

    // Real input bars of the requested timeframe: one delivery per input bar,
    // and the same ceiling whether a chart bar holds 16 or 240 of them.
    const Work real_coarse = measure("15", 16, chart_bars, "15", 15);
    const Work real_fine = measure("1", 240, chart_bars, "1", 1);
    if (real_coarse.lookups > 4 * chart_bars || real_fine.lookups > 4 * chart_bars)
        print_work("real 15m against 1m input", real_coarse, real_fine);
    assert(real_coarse.lookups > 0 && real_coarse.lookups <= 4 * chart_bars);
    assert(real_fine.lookups > 0 && real_fine.lookups <= 4 * chart_bars);
}

} // namespace

int main() {
    lower_tf_caller_work_controls();
    interleaved_catalog_work();
}
