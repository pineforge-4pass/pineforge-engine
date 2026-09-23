// R5 lane PERF-K3. The matcher's cost per driver point is linear in the
// number of live requests, not quadratic.
//
// At every driver point match_path refreshes the allowance of each live
// request that did not get one in bulk -- every Transact, every owner-bound
// close -- and each refresh is a winner of its own. Until this lane every
// such winner was followed by a rescan of the WHOLE live book, although a
// refresh at an unmoved cursor changes nothing but the winner's own
// allowance, so K resting orders cost K full scans of K rows at every point.
// Lane PERF0-K measured it on spark (bare kernel, FeedTolerant labels):
// 10 resting limits 9.5 us/bar, 20 30.5, 50 172, 100 626.
//
// The witness is the SHAPE of the cost, like
// test_native_continuation_digest_tail.cpp. Measured on this TU (macOS
// arm64): on fc7aad62, 10 live 0.0220 s and 40 live 0.2606 s, a ratio of
// 11.9 for four times the orders; once a refresh rescans only its winner,
// 0.0074 s and 0.0277 s, a ratio of 3.7 (under four: the per-bar work every
// run pays is a larger share of the smaller leg). 8 lies between the two
// shapes, about twice the linear one and 1.5 times under the quadratic
// one. The sample is process CPU time (std::clock), best of three per
// leg. The values the run reaches are pinned elsewhere
// (test_native_match_hash_witness) and compared bit for bit against the
// full rescan (test_native_match_row_reuse); here only the finished state is
// checked, so a leg that failed or placed fewer orders cannot pass as cheap.
#include <pineforge/native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;

int failures = 0;
#define CHECK(condition) do {                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

// K buy limits far below the tape, placed on the first bar and never
// reached: every driver point refreshes all K allowances and fills nothing.
struct Resting final : NativeStrategyHost {
    int live = 0;
    int accepted = 0;
    int bars = 0;
    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        if (bars++ != 0) return;
        for (int index = 0; index < live; ++index) {
            no::Request request{no::Transact{1.0}, "k3-resting", ""};
            request.trigger = no::Limit{bar.close * 0.5 - 0.01 * index};
            if (submit(request).handle) ++accepted;
        }
    }
};

NativeRunSpec resting_spec() {
    NativeRunSpec spec;
    spec.identity = {"k3-rescan-scaling", 1};
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.tickerid = "TEST:K3";
    spec.timezone = "UTC";
    spec.session = "24x7";
    // The labels the Pine adapter always runs under. The default Canonical
    // labels add a calendar cost per bar that is the same for both legs and
    // would only dilute the shape this row measures.
    spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    spec.initial_capital = 1000000;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::Percent;
    spec.fee_value = 0.05;
    return spec;
}

// A deterministic walk in whole cents around 100, never near the orders.
std::vector<Bar> tape(int count) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        const double open = 100.0 + static_cast<double>(index % 11) * 0.25;
        const double close = 100.0 + static_cast<double>((index + 3) % 11) * 0.25;
        bars.push_back({open, (open > close ? open : close) + 0.5,
                        (open < close ? open : close) - 0.5, close, 1.0,
                        1736121600000LL + static_cast<std::int64_t>(index) * 300000});
    }
    return bars;
}

double replay(int live, const std::vector<Bar>& bars) {
    Resting host;
    host.live = live;
    const auto setup = host.configure_native(resting_spec());
    CHECK(setup.status == NativeSetupStatus::Applied);
    const std::clock_t started = std::clock();
    host.run(bars.data(), static_cast<int>(bars.size()));
    const double seconds = static_cast<double>(std::clock() - started) / CLOCKS_PER_SEC;
    CHECK(host.last_error().empty());
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.accepted == live);
    CHECK(static_cast<int>(host.native_working_requests().size()) == live);
    CHECK(host.trade_count() == 0);
    return seconds;
}

double best_of_three(int live, const std::vector<Bar>& bars) {
    double best = 0.0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const double sample = replay(live, bars);
        if (attempt == 0 || sample < best) best = sample;
    }
    return best;
}

constexpr int kBars = 3000;
constexpr int kLive = 10;
constexpr double kShapeBound = 8.0;

void matching_is_linear_in_the_live_requests() {
    const std::vector<Bar> bars = tape(kBars);
    const double small = best_of_three(kLive, bars);
    const double large = best_of_three(kLive * 4, bars);
    const double ratio = small > 0.0 ? large / small : 0.0;
    std::printf("resting orders: %d live %.4fs, %d live %.4fs, ratio %.2f (bound %.1f)\n",
                kLive, small, kLive * 4, large, ratio, kShapeBound);
    CHECK(small > 0.0);
    CHECK(ratio < kShapeBound);
}

} // namespace

int main() {
    matching_is_linear_in_the_live_requests();
    if (failures == 0) std::printf("test_native_match_rescan_scaling: ok\n");
    return failures == 0 ? 0 : 1;
}
