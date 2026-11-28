#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/source/pine_host_reads.hpp"

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
using source::detail::lower_tf_index_lookups_for_test;

constexpr int charts = 3;
constexpr int site = 7;
constexpr int peer_site = 8;
constexpr std::int64_t minute = 60'000;
constexpr std::int64_t origin = 1'704'067'200'000;
using Row = std::tuple<std::uint8_t, std::int64_t, std::int64_t, std::int64_t,
                       std::uint64_t, std::string, std::uint64_t>;
using Callers = std::pair<std::uint64_t, std::uint64_t>;
struct Chart {
    std::vector<double> closes;
    std::vector<Callers> callers;
    std::vector<Row> catalog;
    std::uint64_t next_id = 0;
    std::uint64_t lookups = 0;
};

struct Probe final : source::PineStrategyHost {
    Probe(const char* tf, int slots) : tf(tf), slots(slots) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_type("crypto");
    }

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_lower_tf_eval(site, tf, input_tf_);
        register_security_lower_tf_eval(peer_site, tf, input_tf_);
    }

    void evaluate_security(int id, const Bar& bar, bool publish) override {
        if (id == peer_site) {
            const int peer_call = peer_calls++;
            if (peer_call % slots != 0) return;
            // The first site's batch has finished; the peer's live index is zero.
            if (calls != peer_call + slots) {
                std::fprintf(stderr, "NOT COMPARED: site %d callbacks %d/%d before site %d's batch\n",
                             site, calls, peer_call + slots, peer_site);
                std::exit(EXIT_FAILURE);
            }
            const int other_index = security_lower_tf_sub_bar_index(site);
            if (other_index != slots) {
                std::fprintf(stderr, "FAIL: site %d's first callback read site %d index %d/%d\n",
                             peer_site, site, other_index, slots);
                std::exit(EXIT_FAILURE);
            }
            return;
        }
        // The generated array clears at index zero and appends each close.
        const auto before = lower_tf_index_lookups_for_test;
        const int index = security_lower_tf_sub_bar_index(id);
        chart.lookups += lower_tf_index_lookups_for_test - before;
        const auto delivery = current_request_delivery(id);
        const auto expected_time = origin + calls * (240 * minute / slots);
        if (id != site || index != calls % slots || bar.timestamp != expected_time ||
            delivery.requested_slot != calls || !publish || !delivery.publication_present ||
            !delivery.publication || !delivery.actual_complete || !delivery.caller_valid ||
            delivery.replay || delivery.eligible || request_delivery_stack_.empty()) {
            std::fprintf(stderr, "NOT COMPARED: site %d/%d, index %d/%d, time %lld/%lld, "
                         "slot %lld/%d, publish/present/published/complete/valid/replay/eligible "
                         "%d%d%d%d%d%d%d/1111100, frames %zu/at least 1\n",
                         id, site, index, calls % slots, static_cast<long long>(bar.timestamp),
                         static_cast<long long>(expected_time), static_cast<long long>(delivery.requested_slot),
                         calls, publish, delivery.publication_present, delivery.publication,
                         delivery.actual_complete, delivery.caller_valid, delivery.replay,
                         delivery.eligible, request_delivery_stack_.size());
            std::exit(EXIT_FAILURE);
        }
        if (index == 0) chart.closes.clear();
        chart.closes.push_back(bar.close);
        chart.callers.emplace_back(delivery.caller, request_delivery_stack_.back().body_caller);
        ++calls;
    }

    void on_source_bar(const Bar& bar) override {
        const auto expected_time = origin + static_cast<std::int64_t>(snapshots.size()) * 240 * minute;
        if (bar.timestamp != expected_time ||
            chart.closes.size() != static_cast<std::size_t>(slots) ||
            chart.callers.size() != static_cast<std::size_t>(slots)) {
            std::fprintf(stderr, "NOT COMPARED: chart open %lld/%lld, closes %zu/%d, caller pairs %zu/%d\n",
                         static_cast<long long>(bar.timestamp), static_cast<long long>(expected_time),
                         chart.closes.size(), slots, chart.callers.size(), slots);
            std::exit(EXIT_FAILURE);
        }
        chart.next_id = request_next_caller_;
        request_caller_ids_.for_each([&](const auto& key, std::int64_t slot, std::uint64_t caller) {
            chart.catalog.emplace_back(key.kind, key.first, slot, key.third, key.occurrence, key.text, caller);
        });
        snapshots.push_back(std::move(chart));
        chart = {};
    }

    const char* tf;
    int slots, calls = 0, peer_calls = 0;
    Chart chart;
    std::vector<Chart> snapshots;
};

std::vector<Chart> measure(const char* tf, int slots, bool force_lookup, bool force_setter = false) {
    Bar bars[charts];
    for (int i = 0; i < charts; ++i) {
        const double p = 100 + i;
        bars[i] = {p, p + 3, p - 2, p + 1, 480, origin + i * 240 * minute};
    }
    Probe probe(tf, slots);
    auto& force = source::detail::force_lower_tf_index_lookup_for_test;
    auto& setter = source::detail::force_static_delivery_setter_for_test;
    const auto sets_before = source::detail::static_delivery_sets_for_test;
    const bool previous = std::exchange(force, force_lookup);
    const bool previous_setter = std::exchange(setter, force_setter);
    probe.run(bars, charts, "240", "240");
    setter = previous_setter;
    force = previous;
    if (!probe.last_error().empty() || probe.calls != charts * slots ||
        probe.peer_calls != charts * slots || probe.snapshots.size() != charts) {
        std::fprintf(stderr, "NOT COMPARED: TF %s, error '%s'/empty, callbacks %d/%d, "
                     "peer callbacks %d/%d, charts %zu/%d\n",
                     tf, probe.last_error().c_str(), probe.calls, charts * slots,
                     probe.peer_calls, charts * slots, probe.snapshots.size(), charts);
        std::exit(EXIT_FAILURE);
    }
    const auto sets = source::detail::static_delivery_sets_for_test - sets_before;
    const int expected_sets = 2 * charts * slots;
    if (force_setter && sets < static_cast<std::uint64_t>(expected_sets)) {
        std::fprintf(stderr, "NOT COMPARED: TF %s, forced delivery setter calls %llu/at least %d\n",
                     tf, static_cast<unsigned long long>(sets), expected_sets);
        std::exit(EXIT_FAILURE);
    }
    return std::move(probe.snapshots);
}

void compare(const char* tf, int slots) {
    const auto original = measure(tf, slots, true);
    const auto ordinary = measure(tf, slots, false);
    const auto forced_setter = measure(tf, slots, false, true);
    for (int i = 0; i < charts; ++i) {
        const auto& a = original[i];
        const auto& b = ordinary[i];
        if (a.lookups != static_cast<std::uint64_t>(slots) || a.catalog.empty() ||
            a.catalog.size() != b.catalog.size()) {
            std::fprintf(stderr, "NOT COMPARED: TF %s chart %d reference index lookups %llu/%d, "
                         "catalog sizes %zu/%zu (both must be nonempty and equal)\n", tf, i,
                         static_cast<unsigned long long>(a.lookups), slots, a.catalog.size(), b.catalog.size());
            std::exit(EXIT_FAILURE);
        }
        if (std::memcmp(a.closes.data(), b.closes.data(), slots * sizeof(double)) != 0 ||
            a.callers != b.callers || a.catalog != b.catalog || a.next_id != b.next_id) {
            std::fprintf(stderr, "FAIL: TF %s chart %d delivered closes, caller pairs, catalog rows "
                         "or next caller differ (next %llu/%llu)\n", tf, i,
                         static_cast<unsigned long long>(a.next_id), static_cast<unsigned long long>(b.next_id));
            std::exit(EXIT_FAILURE);
        }
        if (b.lookups != 0 || forced_setter[i].lookups != 0) {
            std::fprintf(stderr, "FAIL: TF %s chart %d same-site index lookups "
                         "(ordinary/forced setter) %llu/%llu, expected 0/0 (%d deliveries)\n",
                         tf, i, static_cast<unsigned long long>(b.lookups),
                         static_cast<unsigned long long>(forced_setter[i].lookups), slots);
            std::exit(EXIT_FAILURE);
        }
        std::printf("TF %s chart %d: same-site index lookups (reference/ordinary/forced setter) %llu/%llu/%llu, "
                    "%d closes and caller pairs, %zu catalog rows and next caller %llu "
                    "compared between reference and ordinary\n",
                    tf, i, static_cast<unsigned long long>(a.lookups),
                    static_cast<unsigned long long>(b.lookups), static_cast<unsigned long long>(forced_setter[i].lookups),
                    slots, a.catalog.size(), static_cast<unsigned long long>(a.next_id));
    }
}
} // namespace

int main() {
    compare("1", 240);
    compare("5", 48);
}
