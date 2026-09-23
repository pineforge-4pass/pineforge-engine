// R5 lane PERF-P23: the callback plumbing's caches against the lookups they
// replace, bit for bit.
//
// NativeExecutionConsumer used to re-derive, at every callback site, a fact
// that does not change while the consumer lives: the engine's
// NativeStrategyHost view, a dynamic_cast at 18 sites, now taken once per
// public begin (native_host()). The cached answer must be dynamic_cast's at
// every moment a host can observe. This witness drives randomized hosts --
// two host classes, specs over timeframe pairings, label policies, undetected
// timeframes, cadences and intrabar paths, and lifecycles through batch runs,
// streams, refusals, cooperative aborts, callback exceptions and reuse -- and
// compares, inside every host hook and around every public call, the cache
// with dynamic_cast itself. It also asks one host's consumer about other
// engines, native and not, which the cache must answer as dynamic_cast does.
//
// Fail-before: at the lane's base the consumer has no native_host(), so this
// TU does not compile there (the lane report records the first diagnostic).
//
// Source-free: this TU runs in the kernel-only profile.
#include "../src/native_execution_consumer.hpp"

#include <pineforge/native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int passed = 0;
int failed = 0;
std::uint64_t comparisons = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            ++passed;                                                            \
        } else {                                                                 \
            ++failed;                                                            \
            std::fprintf(stderr, "CHECK FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

// A failed comparison is reported with where it happened; a passing one is
// only counted, so the matrix stays quiet.
#define SAME(where, a, b)                                                        \
    do {                                                                         \
        ++comparisons;                                                           \
        if ((a) == (b)) {                                                        \
            ++passed;                                                            \
        } else {                                                                 \
            ++failed;                                                            \
            std::fprintf(stderr, "MISMATCH at %s: %s != %s\n", where, #a, #b);    \
        }                                                                        \
    } while (0)

constexpr std::int64_t kT0 = 1704067200000LL;  // 2024-01-01 00:00 UTC
constexpr std::int64_t kMinute = 60000;

// A non-native engine: the consumer's host view of it is null.
struct PlainEngine final : BacktestEngine {
    PlainEngine() : BacktestEngine(NativeConsumerBindTag{}) {}
    void on_bar(const Bar&) override {}
};

struct CacheHost : NativeStrategyHost {
    // Other engines this host's consumer is asked about.
    std::vector<BacktestEngine*> others;
    std::uint64_t hooks = 0;
    int closes = 0;
    int abort_at = -1;
    int throw_at = -1;

    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    const NativeExecutionConsumer& consumer() const {
        return static_cast<const NativeExecutionConsumer&>(execution_consumer());
    }

    void verify(const char* where) const {
        const NativeExecutionConsumer& c = consumer();
        // The host view, as a mutable engine, a const one, and for every
        // other engine this consumer is asked about.
        auto& self = const_cast<CacheHost&>(*this);
        BacktestEngine& engine = self;
        const BacktestEngine& const_engine = *this;
        SAME(where, c.native_host(engine), dynamic_cast<NativeStrategyHost*>(&engine));
        SAME(where, c.native_host(const_engine),
             dynamic_cast<const NativeStrategyHost*>(&const_engine));
        for (BacktestEngine* other : others) {
            SAME(where, c.native_host(*other), dynamic_cast<NativeStrategyHost*>(other));
            const BacktestEngine& const_other = *other;
            SAME(where, c.native_host(const_other),
                 dynamic_cast<const NativeStrategyHost*>(&const_other));
        }
    }

    void hook(const char* where) {
        ++hooks;
        verify(where);
    }

    void prepare_native_begin(const NativeBeginArgs&) override { hook("prepare_native_begin"); }
    void on_native_run_begin() override { hook("on_native_run_begin"); }
    void on_native_input(const Bar&, const NativeInputContext&) override {
        hook("on_native_input");
    }
    void on_native_tick(const Bar&, const NativeTickContext&) override {
        hook("on_native_tick");
    }
    void on_native_timeframe_bar(const Bar&, const NativeTimeframeBarContext&) override {
        hook("on_native_timeframe_bar");
    }
    void on_native_bar_open(const Bar&, const NativeDecisionContext&) override {
        hook("on_native_bar_open");
    }
    void on_native_recalculate(const Bar& bar, const NativeDecisionContext& ctx,
                               NativeCalculationReason reason,
                               const no::ExecutionAppliedEvent* cause) override {
        hook("on_native_recalculate");
        if (reason != NativeCalculationReason::BarClose) return;
        NativeStrategyHost::on_native_recalculate(bar, ctx, reason, cause);
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        hook("on_native_bar");
        const int index = closes++;
        if (index == abort_at) request_abort();
        if (index == throw_at) throw std::runtime_error("cache witness callback exception");
        if (index % 3 == 0) (void)submit({no::Transact{index % 2 == 0 ? 1.0 : -1.0}, "e", ""});
        if (index % 3 == 2) (void)submit({no::Flatten{}, "x", ""});
    }
    void on_native_sub_bar(const Bar&, const NativeDecisionContext&) override {
        hook("on_native_sub_bar");
    }
    void on_native_applied(const no::ExecutionAppliedEvent&,
                           const NativeDecisionContext&) override {
        hook("on_native_applied");
    }
    no::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const override {
        verify("resolve_execution_terms");
        return NativeStrategyHost::resolve_execution_terms(facts);
    }
    NativePrecommitVerdict validate_execution_precommit(
            const NativePrecommitView& view) const override {
        verify("validate_execution_precommit");
        return NativeStrategyHost::validate_execution_precommit(view);
    }
    bool margin_check_allowed(const NativeMarginCheckPoint& point) const override {
        verify("margin_check_allowed");
        return NativeStrategyHost::margin_check_allowed(point);
    }
    bool owns_lot_excursions() const noexcept override {
        verify("owns_lot_excursions");
        return false;
    }
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        verify("hash_host_extension");
        NativeStrategyHost::hash_host_extension(sink);
    }
};

// A deeper, final host class: the view is the same subobject whatever the
// most-derived type.
struct DeeperHost final : CacheHost {
    int extra = 0;
    void on_native_input(const Bar& bar, const NativeInputContext& context) override {
        ++extra;
        CacheHost::on_native_input(bar, context);
    }
};

struct Shape {
    std::string input_tf;
    std::string script_tf;
};

const Shape kShapes[] = {{"1", "1"}, {"1", "5"}, {"5", "5"}, {"15", "60"}, {"", ""}};

NativeRunSpec random_spec(std::mt19937_64& rng, const Shape& shape, std::uint64_t run_number) {
    NativeRunSpec s;
    s.identity = {"perf-p23-caches", run_number};
    s.input_tf = shape.input_tf;
    s.script_tf = shape.script_tf;
    s.timeframe_undetected = shape.input_tf.empty();
    s.slot_label_policy = rng() % 2 == 0 ? NativeSlotLabelPolicy::Canonical
                                         : NativeSlotLabelPolicy::FeedTolerant;
    s.ticker = "MOCK";
    s.tickerid = "TEST:MOCK";
    s.type = "crypto";
    s.currency = "USD";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 10000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.5;
    if (rng() % 3 == 0) s.calculation = NativeCalculationTrigger::BarCloseAndFills;
    if (rng() % 4 == 0) s.report_policy = NativeReportPolicy::KernelRecorded;
    if (!s.timeframe_undetected && rng() % 3 == 0) {
        IntrabarPath::synthesized path;
        s.intrabar.value = path;
    }
    return s;
}

std::vector<Bar> bars_for(const Shape& shape, int n) {
    const std::int64_t step = shape.input_tf == "5" ? 5 * kMinute
                            : shape.input_tf == "15" ? 15 * kMinute : kMinute;
    std::vector<Bar> bars;
    for (int i = 0; i < n; ++i) {
        const double p = 100.0 + (i % 7) - 3.0 * (i % 2);
        bars.push_back(Bar{p, p + 1.5, p - 1.5, p + 0.5, 3.0, kT0 + i * step});
    }
    return bars;
}

struct Tally {
    int runs = 0;
    int completed = 0;
    int failed = 0;
    int streams = 0;
    int refused = 0;
};

// One randomized lifecycle on one host: up to three runs of one shape, each a
// batch run or a stream, some aborted from a callback, some thrown out of one.
void lifecycle(CacheHost& host, std::mt19937_64& rng, Tally& tally,
               std::vector<BacktestEngine*> others) {
    host.others = std::move(others);
    host.verify("fresh host");
    const Shape& shape = kShapes[rng() % (sizeof(kShapes) / sizeof(kShapes[0]))];
    const int runs = 1 + static_cast<int>(rng() % 3);
    for (int run = 0; run < runs; ++run) {
        const NativeRunSpec spec = random_spec(rng, shape, static_cast<std::uint64_t>(run + 1));
        const auto setup = host.configure_native(spec);
        host.verify("after configure_native");
        if (setup.status != NativeSetupStatus::Applied) {
            ++tally.refused;
            break;
        }
        host.closes = 0;
        host.abort_at = rng() % 5 == 0 ? static_cast<int>(rng() % 6) : -1;
        host.throw_at = host.abort_at < 0 && rng() % 7 == 0 ? static_cast<int>(rng() % 6) : -1;
        const bool undetected = spec.timeframe_undetected;
        const int n = undetected ? 1 : 6 + static_cast<int>(rng() % 10);
        const auto bars = bars_for(shape, n);
        ++tally.runs;
        if (!undetected && rng() % 3 == 0) {
            ++tally.streams;
            const int warmup = 2;
            if (host.stream_begin(bars.data(), warmup, shape.input_tf, shape.script_tf)) {
                host.verify("after stream_begin");
                for (int i = warmup; i < n; ++i) {
                    if (!host.stream_push_bar(bars[i])) break;
                    host.verify("after stream_push_bar");
                }
                (void)host.stream_end(false);
            }
            host.verify("after stream_end");
        } else {
            host.run(bars.data(), n);
            host.verify("after run");
        }
        const auto kind = host.native_state().kind;
        if (kind == NativeLifecycleKind::Completed) ++tally.completed;
        if (kind == NativeLifecycleKind::Failed) {
            ++tally.failed;
            // A cooperative abort is recoverable: the next configure reuses
            // the host. A callback exception is not, and the refusal below
            // is itself a state to compare in.
            if (host.native_state().failure.code != NativeFailureCode::Aborted) break;
        }
    }
    host.verify("end of lifecycle");
}

void caches_answer_as_the_lookups_did() {
    std::mt19937_64 rng(0x9E3779B97F4A7C15ULL);
    Tally tally;
    PlainEngine plain;
    for (int round = 0; round < 240; ++round) {
        // Two hosts alive at once, each asked about the other and about a
        // non-native engine.
        if (round % 2 == 0) {
            CacheHost a;
            DeeperHost b;
            lifecycle(a, rng, tally, {&b, &plain});
            lifecycle(b, rng, tally, {&a, &plain});
        } else {
            DeeperHost a;
            CacheHost b;
            lifecycle(a, rng, tally, {&b, &plain});
            lifecycle(b, rng, tally, {&a, &plain});
        }
    }
    std::printf("  %d runs (%d streams): %d completed, %d failed, %d configure refusals;"
                " %llu comparisons\n",
                tally.runs, tally.streams, tally.completed, tally.failed, tally.refused,
                static_cast<unsigned long long>(comparisons));
    // Not vacuous: every lifecycle branch was taken.
    CHECK(tally.completed > 100);
    CHECK(tally.failed > 10);
    CHECK(tally.streams > 50);
    CHECK(comparisons > 100000);
}

}  // namespace

int main() {
    caches_answer_as_the_lookups_did();
    std::printf("test_native_callback_caches: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
