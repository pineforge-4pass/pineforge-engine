// Count the real bar-open callback over unchanged live books.
// No elapsed time, expected-count harvesting, or alternative adapter path.
#include <cstdint>

namespace bar_work_obs {
struct Counts {
    std::uint64_t bar_opens = 0;
    std::uint64_t lookup_probes = 0;
    std::uint64_t lookup_comparisons = 0;
    std::uint64_t placement_slots = 0;

    std::uint64_t total() const noexcept {
        return bar_opens + lookup_probes + lookup_comparisons + placement_slots;
    }
};

// Only BarOpenScope activates observation during a host run. Header-inline
// placement reads by setup, the matcher and the assertions remain uncounted.
inline thread_local Counts* destination = nullptr;
inline thread_local Counts* active = nullptr;

inline void note(std::uint64_t Counts::*field) noexcept {
    if (active) ++(active->*field);
}

class RunScope {
public:
    explicit RunScope(Counts& counts) noexcept : previous_(destination) {
        destination = &counts;
    }
    ~RunScope() { destination = previous_; }
    RunScope(const RunScope&) = delete;
    RunScope& operator=(const RunScope&) = delete;
private:
    Counts* previous_;
};

class BarOpenScope {
public:
    BarOpenScope() noexcept : previous_(active) {
        active = destination;
        note(&Counts::bar_opens);
    }
    ~BarOpenScope() { active = previous_; }
    BarOpenScope(const BarOpenScope&) = delete;
    BarOpenScope& operator=(const BarOpenScope&) = delete;
private:
    Counts* previous_;
};
} // namespace bar_work_obs

// Like test_adapter_cohort_origin_release.cpp, compile the real adapter TU
// into this executable before resolving the archive. No class layout changes
// or extra source-layer library, and the ordinary library's probes are empty.
// This target defines NDEBUG to count the production decision path; its own
// CHECK/require assertions below remain active in every build profile.
#define PINEFORGE_PLACEMENT_WORK(field) \
    ::bar_work_obs::note(&::bar_work_obs::Counts::field)
#include <pineforge/source/pine_strategy_host.hpp>
#define PINEFORGE_BAR_OPEN_OBSERVE() \
    ::bar_work_obs::BarOpenScope bar_open_scope_guard
#include "../src/source/pine_adapter.cpp"
#undef PINEFORGE_BAR_OPEN_OBSERVE
#undef PINEFORGE_PLACEMENT_WORK

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
using Counts = bar_work_obs::Counts;
using Handle = native_order::RequestHandle;

constexpr int kPopulation = 256;
constexpr int kBars = 16; // Two equal measured windows; setup is outside both.
constexpr int kWarmup = 4;
constexpr int kFeedBars = kWarmup + 2 * kBars + 2;
constexpr std::int64_t kStart = INT64_C(1736121600000);
constexpr double kLimit = 50.0;
// With mintick 1, a buy at source limit 50 reaches raw prints whose half-up
// tick is <= 50. The inclusive native boundary is the last binary64 below
// 50.5: 50.5 itself rounds to 51. This oracle uses only the fixture inputs,
// never the adapter's threshold helper or a price read from the working book.
const double kNativeLimit = std::nextafter(kLimit + 0.5, 0.0);
constexpr double kMissing = std::numeric_limits<double>::quiet_NaN();
// A fixed allowance of eight additional primitive observations per opening,
// irrespective of N. It permits small fixed bookkeeping after a quiet gate.
constexpr std::uint64_t kExtraPerBar = 8;

int failures = 0;
#define CHECK(condition) do {                                                 \
    if (!(condition)) {                                                       \
        ++failures;                                                           \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
    }                                                                         \
} while (false)

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void print_pending_trigger(int bar, std::size_t row, const native_order::Trigger& trigger) {
    std::fprintf(stderr, "BARWORK_TRIGGER bar=%d row=%zu variant=%zu"
                 " expected_price=%.17g expected_hex=%a",
                 bar, row, trigger.index(), kNativeLimit, kNativeLimit);
    if (const auto* limit = std::get_if<native_order::Limit>(&trigger)) {
        std::fprintf(stderr, " kind=Limit price=%.17g price_hex=%a fill_through=%d\n",
                     limit->price, limit->price, static_cast<int>(limit->fill_through));
    } else if (const auto* stop = std::get_if<native_order::Stop>(&trigger)) {
        std::fprintf(stderr, " kind=Stop price=%.17g price_hex=%a\n",
                     stop->price, stop->price);
    } else if (const auto* stop_limit = std::get_if<native_order::StopLimit>(&trigger)) {
        std::fprintf(stderr, " kind=StopLimit stop=%.17g stop_hex=%a"
                     " limit=%.17g limit_hex=%a\n",
                     stop_limit->stop, stop_limit->stop, stop_limit->limit, stop_limit->limit);
    } else if (const auto* trail = std::get_if<native_order::Trail>(&trigger)) {
        const double arm = trail->arm_price.value_or(kMissing);
        const double ticks = trail->ticks ? trail->ticks->ticks : kMissing;
        const double seed = trail->best_seed.value_or(kMissing);
        std::fprintf(stderr, " kind=Trail offset=%.17g offset_hex=%a"
                     " arm_present=%d arm_price=%.17g arm_hex=%a"
                     " ticks_present=%d ticks=%.17g ticks_hex=%a"
                     " seed_present=%d best_seed=%.17g seed_hex=%a\n",
                     trail->offset, trail->offset,
                     static_cast<int>(trail->arm_price.has_value()), arm, arm,
                     static_cast<int>(trail->ticks.has_value()), ticks, ticks,
                     static_cast<int>(trail->best_seed.has_value()), seed, seed);
    } else if (std::holds_alternative<native_order::Market>(trigger)) {
        std::fprintf(stderr, " kind=Market\n");
    } else {
        std::fprintf(stderr, " kind=unrecognized\n");
    }
}

Counts difference(const Counts& after, const Counts& before) {
    require(after.bar_opens >= before.bar_opens
            && after.lookup_probes >= before.lookup_probes
            && after.lookup_comparisons >= before.lookup_comparisons
            && after.placement_slots >= before.placement_slots,
            "bar-open observer moved backwards");
    return {after.bar_opens - before.bar_opens,
            after.lookup_probes - before.lookup_probes,
            after.lookup_comparisons - before.lookup_comparisons,
            after.placement_slots - before.placement_slots};
}

void add(Counts& sum, const Counts& row) {
    sum.bar_opens += row.bar_opens;
    sum.lookup_probes += row.lookup_probes;
    sum.lookup_comparisons += row.lookup_comparisons;
    sum.placement_slots += row.placement_slots;
}

bool equal(const Counts& left, const Counts& right) {
    return left.bar_opens == right.bar_opens
        && left.lookup_probes == right.lookup_probes
        && left.lookup_comparisons == right.lookup_comparisons
        && left.placement_slots == right.placement_slots;
}

struct Sample {
    std::vector<Counts> bars;

    Counts window(int first) const {
        Counts result;
        for (int i = first; i < first + kBars; ++i)
            add(result, bars.at(static_cast<std::size_t>(i)));
        return result;
    }
};

class PendingHost final : public source::PineStrategyHost {
public:
    PendingHost(int population, Counts& observed)
        : population_(population), observed_(observed) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 2 * kPopulation + 1;
        config.margin_long = config.margin_short = 0.0;
        config.commission_value = 0.0;
        config.slippage = 0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(1.0);
        set_broker_state_hash_recording(false);
    }

    Sample sample;
    int source_bars = 0;

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        ++source_bars;
        // on_bar_open has returned: these population checks cannot contribute
        // any probe to the cost under test.
        require(bar_work_obs::active == nullptr, "observer leaked beyond bar open");
        if (i == 0) {
            for (int order = 0; order < population_; ++order)
                strategy_entry("pending-" + std::to_string(order), true,
                               kLimit, kMissing, 1.0);
        } else {
            const auto working = native_working_requests();
            require(working.size() == static_cast<std::size_t>(population_),
                    "native pending population changed");
            const auto population = adapter_.retirement_population();
            require(population.live == static_cast<std::uint64_t>(population_)
                    && population.R == 0 && population.pending == 0,
                    "adapter does not retain exactly N live placements");
            require(trade_count() == 0 && live_position_size() == 0.0,
                    "an unreachable limit filled");
            if (i == 1) {
                for (const auto& row : working) {
                    handles_.push_back(row.definition->handle);
                    definitions_.push_back(row.definition);
                }
            }
            for (std::size_t row = 0; row < working.size(); ++row) {
                const auto& definition = *working[row].definition;
                // Always print before the trigger checks, including on bar 1.
                // Decimal and hex preserve a half-tick boundary's exact value.
                print_pending_trigger(i, row, definition.request.trigger);
                require(definition.handle == handles_.at(row),
                        "a pending order was cancelled or replaced");
                const auto* limit = std::get_if<native_order::Limit>(&definition.request.trigger);
                require(limit != nullptr, "pending request is not a native limit");
                require(limit->price == kNativeLimit, "pending limit threshold changed");
                require(!limit->fill_through, "pending limit lost its bounded fill price");
                // A keep_handle replacement can preserve both the handle and
                // the trigger. The immutable accepted definition must stay too.
                require(working[row].definition == definitions_.at(row),
                        "a pending order was replaced in place");
            }
        }
        if (i >= kWarmup && i < kWarmup + 2 * kBars)
            sample.bars.push_back(difference(observed_, previous_));
        previous_ = observed_;
    }

private:
    int population_;
    Counts& observed_;
    Counts previous_{};
    std::vector<Handle> handles_;
    std::vector<native_order::DefinitionRef> definitions_;
};

Sample replay(int population) {
    Counts observed;
    PendingHost host(population, observed);
    bar_work_obs::RunScope counting(observed);
    std::vector<Bar> feed;
    for (int bar = 0; bar < kFeedBars; ++bar)
        feed.push_back({100.0, 101.0, 99.0, 100.0, 1.0,
                        kStart + static_cast<std::int64_t>(bar) * 60000});
    host.run(feed.data(), static_cast<int>(feed.size()), "1", "1");
    if (!host.last_error().empty())
        throw std::runtime_error("pending host: " + host.last_error());
    require(host.native_state().kind == NativeLifecycleKind::Completed, "run did not complete");
    require(host.source_bars == kFeedBars, "source did not publish every feed bar");
    require(host.sample.bars.size() == static_cast<std::size_t>(2 * kBars),
            "observer did not cover both complete windows");
    for (const auto& bar : host.sample.bars) {
        require(bar.bar_opens == 1, "sample is not one real bar-open callback");
    }
    for (int window = 0; window < 2; ++window) {
        const auto work = host.sample.window(window * kBars);
        std::printf("BARWORK_WORK N=%d K=%d window=%d opens=%" PRIu64
                    " probes=%" PRIu64 " comparisons=%" PRIu64
                    " slots=%" PRIu64 " total=%" PRIu64 " per_bar=%.6f\n",
                    population, kBars, window, work.bar_opens, work.lookup_probes,
                    work.lookup_comparisons, work.placement_slots, work.total(),
                    static_cast<double>(work.total()) / kBars);
    }
    std::fflush(stdout);
    return host.sample;
}

void doubling() {
    const auto small = replay(kPopulation).window(0);
    const auto large = replay(2 * kPopulation).window(0);
    const double ratio = static_cast<double>(large.total()) / small.total();
    std::printf("BARWORK_DOUBLING ratio=%.9f flat_bound=1.25 linear_bound=2.25"
                " quadratic_reference=4 small=%" PRIu64 " large=%" PRIu64 "\n",
                ratio, small.total(), large.total());
    // Integer inequalities: no timing, floating threshold or observed oracle.
    CHECK(4 * large.total() <= 9 * small.total());
    CHECK(4 * large.total() <= 5 * small.total());
}

void absolute() {
    const auto empty = replay(0).window(0);
    const auto sample = replay(kPopulation);
    const auto pending = sample.window(0);
    const auto allowance = static_cast<std::uint64_t>(kBars) * kExtraPerBar;
    const auto maximum = static_cast<std::uint64_t>(kBars) + allowance;
    std::uint64_t peak = 0;
    for (const auto& bar : sample.bars) peak = std::max(peak, bar.total());
    std::printf("BARWORK_ABSOLUTE N=%d K=%d empty=%" PRIu64 " pending=%" PRIu64
                " extra_allowance=%" PRIu64 " maximum=%" PRIu64
                " peak_per_bar=%" PRIu64 "\n",
                kPopulation, kBars, empty.total(), pending.total(), allowance,
                maximum, peak);
    CHECK(empty.bar_opens == static_cast<std::uint64_t>(kBars));
    CHECK(empty.total() == static_cast<std::uint64_t>(kBars));
    CHECK(pending.total() >= static_cast<std::uint64_t>(kBars));
    CHECK(pending.total() <= maximum);
    CHECK(peak <= 1 + kExtraPerBar);
}

void control() {
    // Independently calibrate the primitive counter on real dense placements.
    // This tests instrumentation, not the desired cost of on_bar_open: step 2
    // is free to skip all N lookups there and this control will still pass.
    source::PlacementTable table;
    for (int order = 1; order <= kPopulation; ++order)
        table.try_emplace(static_cast<std::uint64_t>(order));
    Counts calibration;
    {
        struct ProbeScope {
            Counts* previous = bar_work_obs::active;
            explicit ProbeScope(Counts& counts) { bar_work_obs::active = &counts; }
            ~ProbeScope() { bar_work_obs::active = previous; }
        } probes(calibration);
        for (int order = 1; order <= kPopulation; ++order)
            require(table.find(static_cast<std::uint64_t>(order)) != table.end(),
                    "calibration lookup lost a real placement");
        require(table.find(static_cast<std::uint64_t>(kPopulation) + 1) == table.end(),
                "calibration miss unexpectedly hit");
        std::uint64_t visited = 0;
        table.visit_rows([&](std::uint64_t, const source::PlacementSnapshot&) {
            ++visited;
            return false;
        });
        require(visited == static_cast<std::uint64_t>(kPopulation),
                "calibration did not visit every row");
    }
    CHECK(calibration.lookup_probes == static_cast<std::uint64_t>(kPopulation) + 1);
    CHECK(calibration.lookup_comparisons == 0);
    CHECK(calibration.placement_slots == static_cast<std::uint64_t>(kPopulation));
    CHECK(calibration.bar_opens == 0);
    std::printf("BARWORK_CALIBRATION N=%d probes=%" PRIu64 " slots=%" PRIu64 "\n",
                kPopulation, calibration.lookup_probes, calibration.placement_slots);

    const auto sample = replay(kPopulation);
    const auto first = sample.window(0);
    const auto second = sample.window(kBars);
    std::printf("BARWORK_CONTROL N=%d K=%d first=%" PRIu64 " second=%" PRIu64 "\n",
                kPopulation, kBars, first.total(), second.total());
    CHECK(equal(first, second));
    for (int bar = 0; bar < kBars; ++bar)
        CHECK(equal(sample.bars.at(static_cast<std::size_t>(bar)),
                    sample.bars.at(static_cast<std::size_t>(bar + kBars))));
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 1) {
            doubling();
            absolute();
            control();
        } else if (argc == 2 && std::strcmp(argv[1], "doubling") == 0) {
            doubling();
        } else if (argc == 2 && std::strcmp(argv[1], "absolute") == 0) {
            absolute();
        } else if (argc == 2 && std::strcmp(argv[1], "control") == 0) {
            control();
        } else {
            throw std::runtime_error("expected doubling, absolute, control, or no argument");
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL test_adapter_bar_work: %s\n", error.what());
        return 1;
    }
    std::printf("test_adapter_bar_work: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
