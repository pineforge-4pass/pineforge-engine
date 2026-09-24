// R5 lane D2-A (PQ2, first half): the magnifier sampler handed its leg order
// and keeping no scratch, against the sampler as it stood, bit for bit.
//
// Until this lane the intrabar driver installed its run's declared leg order
// (NativeRunSpec::path_order) in a thread-local override around the sampler
// (NativePathOrderScope, internal::set_path_order_override), and the sampler
// kept three thread_local scratch vectors; in a strategy loaded with dlopen
// each access is a TLS-descriptor call. Now the consumer hands the order to
// internal::sample_price_path_ordered, the public forms hand it the
// open-proximity rule, and the t-values are written into the caller's output
// with the mandatory times on the stack. Two witnesses:
//   1. Over randomized bars -- quarter ticks, random magnitudes and signs,
//      every zero-length leg, legs closer than the dedup tolerance, ties,
//      inverted bars, huge, tiny and subnormal prices, NaN and infinities in
//      each field -- every sample count (the clamped ones included), every
//      distribution and both leg orders: the ordered form gives the bits of
//      the base routine (tests/magnifier_base_routine.hpp), and so do the
//      public forms, value-returning and out-parameter, and the
//      volume-weighted forms.
//   2. For every declared order, the consumer's resolution
//      (NativeExecutionConsumer::path_high_first, the order the sampler is
//      handed) answers what the thread-local override answered under that
//      order: HighFirst, LowFirst, and the open-proximity rule under Auto.
//   3. Runs under each declared order, with a synthesized and a
//      lower-timeframe path, walk exactly the base routine's samples in that
//      order: every driver point's price, in sequence, is the samples of its
//      script bar or the corners of its sub-bar.
//
// Fail-before: at the lane's base engine_internal.hpp declares neither
// sample_price_path_ordered nor sample_price_path_volume_weighted_ordered, so
// this TU does not compile there (the lane report records the first
// diagnostic).
//
// Source-free: this TU runs in the kernel-only profile.
#include "../src/native_execution_consumer.hpp"
#include "magnifier_base_routine.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace pineforge;

namespace {
namespace base = magnifier_base;

int failures = 0;
long checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

void sampler_matches_the_base_routine() {
    const std::vector<Bar> bars = base::sampler_bars();
    const MagnifierDistribution distributions[] = {
        MagnifierDistribution::UNIFORM, MagnifierDistribution::COSINE,
        MagnifierDistribution::TRIANGLE, MagnifierDistribution::ENDPOINTS,
        MagnifierDistribution::FRONT_LOADED, MagnifierDistribution::BACK_LOADED};
    const int counts[] = {-3, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 33, 64};
    long ordered = 0;
    std::vector<double> out;
    for (std::size_t index = 0; index < bars.size(); ++index) {
        const Bar& bar = bars[index];
        const bool proximity = std::abs(bar.high - bar.open) < std::abs(bar.open - bar.low);
        // Every bar at four ENDPOINTS samples, both orders; one bar in five
        // at every other count and distribution.
        for (const MagnifierDistribution dist : distributions) {
            for (const int n : counts) {
                const bool everything = index % 5 == 0;
                if (!everything && !(n == 4 && dist == MagnifierDistribution::ENDPOINTS)) {
                    continue;
                }
                for (const bool high_first : {false, true}) {
                    internal::sample_price_path_ordered(bar, high_first, n, dist, out);
                    CHECK(base::same_bits(out, base::sample(bar, high_first, n, dist)));
                    ++ordered;
                }
                const std::vector<double> reference = base::sample(bar, proximity, n, dist);
                sample_price_path(bar, n, dist, out);
                CHECK(base::same_bits(out, reference));
                CHECK(base::same_bits(sample_price_path(bar, n, dist), reference));
            }
            if (index % 5 != 0) continue;
            for (const double mean : {0.0, 1.0, 3.5}) {
                const int n = base::volume_count(bar, 4, mean, 2, 64);
                for (const bool high_first : {false, true}) {
                    internal::sample_price_path_volume_weighted_ordered(
                        bar, high_first, 4, mean, 2, 64, dist, out);
                    CHECK(base::same_bits(out, base::sample(bar, high_first, n, dist)));
                }
                const std::vector<double> reference = base::sample(bar, proximity, n, dist);
                sample_price_path_volume_weighted(bar, 4, mean, 2, 64, dist, out);
                CHECK(base::same_bits(out, reference));
                CHECK(base::same_bits(sample_price_path_volume_weighted(bar, 4, mean, 2, 64, dist),
                                      reference));
            }
        }
    }
    std::printf("sampler: %zu bars, %ld ordered samplings equal to the base routine\n",
                bars.size(), ordered);
}

class OrderHost final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

void consumer_hands_the_declared_order() {
    long compared = 0;
    const std::vector<Bar> bars = base::sampler_bars();
    for (const NativePathOrder order :
         {NativePathOrder::Auto, NativePathOrder::HighFirst, NativePathOrder::LowFirst}) {
        k3_book::BookConfig config;
        config.bars = 3;
        const k3_book::Tape tape = k3_book::make_tape(config);
        NativeRunSpec spec = k3_book::make_spec(config, tape);
        spec.path_order = order;
        OrderHost host;
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        const auto& consumer = NativeExecutionConsumer::bound(host);
        for (const Bar& bar : bars) {
            CHECK(consumer.path_high_first(bar) == base::override_high_first(order, bar));
            ++compared;
        }
    }
    std::printf("declared orders: %ld resolutions equal to the override's\n", compared);
}

// The prices the driver walked, in order: under a synthesized path each
// script bar's samples, under a lower-timeframe one each sub-bar's corners.
void runs_walk_the_declared_order() {
    long points = 0;
    for (const NativePathOrder order :
         {NativePathOrder::Auto, NativePathOrder::HighFirst, NativePathOrder::LowFirst}) {
        for (const k3_book::Path path : {k3_book::Path::Synthesized, k3_book::Path::Lower}) {
            k3_book::BookConfig config;
            config.seed = 95001 + static_cast<std::uint64_t>(points % 97);
            config.bars = 60;
            config.path = path;
            const k3_book::Tape tape = k3_book::make_tape(config);
            NativeRunSpec spec = k3_book::make_spec(config, tape);
            // Canonical labels, so a lower feed's windows hold its sub-bars.
            spec.slot_label_policy = NativeSlotLabelPolicy::Canonical;
            spec.path_order = order;
            OrderHost host;
            CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
            host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
            CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
            std::vector<double> walked;
            for (const auto& event : host.native_events(0)) {
                if (event.kind == NativeEventKind::Driver) walked.push_back(event.driver->raw_price);
            }
            std::vector<double> expected;
            for (std::size_t bar = 0; bar < tape.bars.size(); ++bar) {
                const std::size_t subs = path == k3_book::Path::Lower ? 5 : 1;
                for (std::size_t k = 0; k < subs; ++k) {
                    const Bar& sub = path == k3_book::Path::Lower ? tape.minutes[bar * 5 + k]
                                                                  : tape.bars[bar];
                    const auto samples = base::sample(
                        sub, base::override_high_first(order, sub), 4,
                        MagnifierDistribution::ENDPOINTS);
                    expected.insert(expected.end(), samples.begin(), samples.end());
                }
            }
            CHECK(base::same_bits(walked, expected));
            points += static_cast<long>(walked.size());
        }
    }
    std::printf("runs: %ld driver points walked in the declared order\n", points);
}

}  // namespace

int main() {
    sampler_matches_the_base_routine();
    consumer_hands_the_declared_order();
    runs_walk_the_declared_order();
    if (failures != 0) {
        std::fprintf(stderr, "test_magnifier_ordered_sampler: %d failure(s) in %ld checks\n",
                     failures, checks);
        return 1;
    }
    std::printf("test_magnifier_ordered_sampler: ok (%ld checks)\n", checks);
    return 0;
}
