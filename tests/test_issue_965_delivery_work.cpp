#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/engine_internal.hpp"
#include "../src/source/pine_host_reads.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace pineforge;
constexpr std::int64_t minute = 60'000;
constexpr std::int64_t origin = 1'704'067'200'000;

struct Work {
    std::uint64_t syntheses;
    std::uint64_t sets;
    std::uint64_t appends;
};

Work counts() {
    return {internal::lower_tf_syntheses_for_test,
            source::detail::static_delivery_sets_for_test,
            source::detail::static_caller_appends_for_test};
}

// Match the logical fields folded by hash_host_extension. The second key
// coordinate is the expanded slot, not the first slot of the physical run.
using Row = std::tuple<std::uint8_t, std::int64_t, std::int64_t, std::int64_t,
                       std::uint64_t, std::string, std::uint64_t>;
struct Chart {
    std::int64_t timestamp;
    std::uint64_t next_id;
    std::vector<Row> catalog;
    std::vector<double> closes;
    Work work;
};
struct Run {
    Work work;
    std::vector<Chart> charts;
};

struct Probe final : source::PineStrategyHost {
    Probe(const char* tf, int slots, bool clear_catalog) : tf(tf), slots(slots), clear_catalog(clear_catalog) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_type("crypto");
    }

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_lower_tf_eval(0, tf, input_tf_);
    }

    void evaluate_security(int id, const Bar& bar, bool publish) override {
        const auto d = current_request_delivery(id);
        const int index = security_lower_tf_sub_bar_index(id);
        const auto timestamp = origin + calls * (240 * minute / slots);
        if (id != 0 || !publish || !d.publication_present || !d.publication ||
            !d.actual_complete || !d.caller_valid || d.eligible || d.replay ||
            d.requested_slot != calls || index != calls % slots || bar.timestamp != timestamp) {
            std::fprintf(stderr, "NOT COMPARED: site %d/0, publish %d/1, present %d/1, "
                         "publication %d/1, complete %d/1, valid %d/1, eligible %d/0, "
                         "replay %d/0, slot %lld/%d, index %d/%d, timestamp %lld/%lld\n",
                         id, publish, d.publication_present, d.publication, d.actual_complete,
                         d.caller_valid, d.eligible, d.replay, static_cast<long long>(d.requested_slot),
                         calls, index, calls % slots, static_cast<long long>(bar.timestamp),
                         static_cast<long long>(timestamp));
            std::exit(EXIT_FAILURE);
        }
        if (index == 0) closes.clear();
        closes.push_back(bar.close);
        // Exercise invalidation while the optimized path holds a borrow,
        // with no intervening ID allocation to trip the adjacency guard.
        if (clear_catalog && calls == 0) request_caller_ids_.clear();
        ++calls;
    }

    void on_source_bar(const Bar& bar) override {
        if (closes.size() != static_cast<std::size_t>(slots) || closes.back() != bar.close) {
            std::fprintf(stderr, "NOT COMPARED: chart %d, closes %zu/%d, last %a/%a\n",
                         charts, closes.size(), slots, closes.empty() ? NAN : closes.back(), bar.close);
            std::exit(EXIT_FAILURE);
        }
        const Work now = counts();
        snapshots.push_back({bar.timestamp, request_next_caller_, {}, closes,
                             {now.syntheses - prior.syntheses, now.sets - prior.sets,
                              now.appends - prior.appends}});
        auto& rows = snapshots.back().catalog;
        request_caller_ids_.for_each([&](const auto& key, std::int64_t slot, std::uint64_t caller) {
            rows.emplace_back(key.kind, key.first, slot, key.third, key.occurrence, key.text, caller);
        });
        prior = now;
        ++charts;
    }

    const char* tf;
    int slots, calls = 0, charts = 0;
    bool clear_catalog;
    std::vector<double> closes;
    Work prior = counts();
    std::vector<Chart> snapshots;
};

Run measure(const char* tf, int slots, int charts, bool force_setter, bool clear_catalog) {
    std::vector<Bar> bars;
    for (int i = 0; i < charts; ++i) {
        const double p = 100.0 + i;
        bars.push_back({p, p + 3, p - 2, p + 1, 480, origin + i * 240 * minute});
    }
    Probe probe(tf, slots, clear_catalog);
    const Work start = probe.prior;
    const bool saved = std::exchange(source::detail::force_static_delivery_setter_for_test, force_setter);
    probe.run(bars.data(), charts, "240", "240");
    source::detail::force_static_delivery_setter_for_test = saved;
    if (!probe.last_error().empty() || probe.calls != charts * slots || probe.charts != charts) {
        std::fprintf(stderr, "NOT COMPARED: error '%s'/empty, deliveries %d/%d, chart bars %d/%d\n",
                     probe.last_error().c_str(), probe.calls, charts * slots, probe.charts, charts);
        std::exit(EXIT_FAILURE);
    }
    const Work end = counts();
    const Work w{end.syntheses - start.syntheses, end.sets - start.sets, end.appends - start.appends};
    std::printf("%s%s, %d chart bars, %d slots/bar: syntheses %llu, static sets %llu, append attempts %llu\n",
                force_setter ? "original setter" : "ordinary", clear_catalog ? " (clear in callback)" : "",
                charts, slots, static_cast<unsigned long long>(w.syntheses),
                static_cast<unsigned long long>(w.sets), static_cast<unsigned long long>(w.appends));
    return {w, std::move(probe.snapshots)};
}

Work compare(const char* tf, int slots, int charts, bool& ok, bool clear_catalog = false) {
    const auto slow = measure(tf, slots, charts, true, clear_catalog);
    const auto fast = measure(tf, slots, charts, false, clear_catalog);
    for (int i = 0; i < charts; ++i) {
        const auto& a = slow.charts[i];
        const auto& b = fast.charts[i];
        const auto n = static_cast<std::uint64_t>(slots);
        if (a.timestamp != b.timestamp || a.work.sets != n || a.work.appends != n ||
            a.catalog.empty() || b.catalog.empty()) {
            std::fprintf(stderr, "NOT COMPARED: chart %d TF %s timestamp %lld/%lld, reference setter %llu/%d, "
                         "reference append %llu/%d, reference/candidate catalog sizes %zu/%zu (both required)\n",
                         i, tf, static_cast<long long>(a.timestamp), static_cast<long long>(b.timestamp),
                         static_cast<unsigned long long>(a.work.sets), slots,
                         static_cast<unsigned long long>(a.work.appends), slots, a.catalog.size(), b.catalog.size());
            std::exit(EXIT_FAILURE);
        }
        if (a.next_id != b.next_id || a.catalog != b.catalog) {
            std::fprintf(stderr, "FAIL: chart %d TF %s caller catalog/next ID differ: rows %zu/%zu, next %llu/%llu\n",
                         i, tf, a.catalog.size(), b.catalog.size(), static_cast<unsigned long long>(a.next_id),
                         static_cast<unsigned long long>(b.next_id));
            ok = false;
        }
        // Both vectors were checked to hold exactly `slots` in on_source_bar.
        if (std::memcmp(a.closes.data(), b.closes.data(), a.closes.size() * sizeof(double)) != 0) {
            std::fprintf(stderr, "FAIL: chart %d TF %s: the %d-element close arrays differ bytewise\n", i, tf, slots);
            ok = false;
        }
        // Structural controls, independent of any v1.12.0 measurement. The
        // ordinary run must actually bypass setter/append calls on this bar.
        if (a.work.syntheses != 1 || b.work.syntheses != 1 ||
            b.work.sets >= a.work.sets || b.work.appends >= a.work.appends) {
            std::fprintf(stderr, "FAIL: chart %d TF %s reference/ordinary syntheses %llu/%llu (one each), "
                         "setter %llu/%llu and append %llu/%llu (ordinary must do less)\n",
                         i, tf, static_cast<unsigned long long>(a.work.syntheses),
                         static_cast<unsigned long long>(b.work.syntheses),
                         static_cast<unsigned long long>(a.work.sets), static_cast<unsigned long long>(b.work.sets),
                         static_cast<unsigned long long>(a.work.appends), static_cast<unsigned long long>(b.work.appends));
            ok = false;
        }
    }
    return fast.work;
}

bool scaled(const Work& a, const Work& b, unsigned factor) {
    return b.syntheses == factor * a.syntheses && b.sets == factor * a.sets
        && b.appends == factor * a.appends;
}

} // namespace

int main() {
    bool ok = true;
    const Work fine = compare("1", 240, 4, ok);
    const Work longer = compare("1", 240, 8, ok);
    const Work coarse = compare("15", 16, 4, ok);
    const Work coarse_longer = compare("15", 16, 8, ok);
    if (!scaled(fine, longer, 2) || !scaled(coarse, coarse_longer, 2) ||
        !scaled(coarse, fine, 1) || !scaled(coarse_longer, longer, 1)) {
        std::fputs("FAIL: work must scale with chart bars and be independent of slot count\n", stderr);
        ok = false;
    }
    compare("15", 16, 4, ok, true);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
