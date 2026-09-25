// R5 lane PERF-L1: the per-point checks and frames against the code they
// replaced.
//
// Three spellings change and nothing they compute may move:
//   1. check_abort and check_abort_or_projection answer a boundary that
//      passes inline, and latch the failure out of line (latch_abort,
//      latch_projection_mismatch). Every boundary a cooperative abort can be
//      caught at -- before and after each input, after each callback kind --
//      must fail with the same NativeFailure and error text. A host requests
//      the abort at the n-th call of each hook, under both reporting modes
//      (Error, Quiet), and the outcome (lifecycle, failure code, operation,
//      ordinal, context kind, error text, trades, continuation) is pinned:
//      harvested at the lane's base (f71cd820, built with -DPF_L1_HARVEST,
//      run with PF_POINT_CHECKS_DUMP=1). Where a projected-field write is
//      caught is pinned hook by hook by tests/test_native_projection_witness.
//   2. margin_sizing_price walks the modeled waypoints without local arrays.
//      It is compared exhaustively with the pre-lane array formula, restated
//      verbatim below: every phase, both sides, both leg orders, fallbacks
//      and waypoint prices drawn from finite, zero, negative, infinite and NaN
//      values, and random bars.
//   3. A callback's current execution frame is constructed in place from its
//      parts. Each parts constructor must build exactly the aggregate the
//      callbacks built before, field for field, over random contexts.
//
// Fail-before: at the lane's base the frame has no parts constructor, so this
// TU does not compile there (the lane report records the first diagnostic);
// the harvest build compiles witness 3 out.
//
// Source-free: this TU runs in the kernel-only profile.
#include "../src/native_execution_consumer.hpp"

#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v19 {

struct NativeExecutionConsumerProbe {
    static void set_margin_path(NativeExecutionConsumer& c, const Bar& bar, bool high_first,
                                bool has_path) {
        c.margin_path_bar_ = bar;
        c.margin_path_high_first_ = high_first;
        c.has_margin_path_ = has_path;
    }
    static double sizing(const NativeExecutionConsumer& c, bool short_side, NativePathPhase phase,
                         double fallback) {
        return c.margin_sizing_price(short_side, phase, fallback);
    }
    // margin_sizing_price at the lane's base, verbatim.
    static double reference_sizing(const NativeExecutionConsumer& c, bool short_side,
                                   NativePathPhase phase, double fallback) {
        if (!c.has_margin_path_) return fallback;
        const NativePathPhase order[4] = {
            NativePathPhase::Open,
            c.margin_path_high_first_ ? NativePathPhase::High : NativePathPhase::Low,
            c.margin_path_high_first_ ? NativePathPhase::Low : NativePathPhase::High,
            NativePathPhase::Close,
        };
        const double prices[4] = {
            c.margin_path_bar_.open,
            c.margin_path_high_first_ ? c.margin_path_bar_.high : c.margin_path_bar_.low,
            c.margin_path_high_first_ ? c.margin_path_bar_.low : c.margin_path_bar_.high,
            c.margin_path_bar_.close,
        };
        int current = -1;
        for (int index = 0; index < 4; ++index) {
            if (order[index] == phase) {
                current = index;
                break;
            }
        }
        if (current < 0) return fallback;
        double adverse = fallback;
        for (int index = current + 1; index < 4; ++index) {
            const double price = prices[index];
            if (!std::isfinite(price) || !(price > 0.0)) continue;
            if (!std::isfinite(adverse) || (short_side ? price > adverse : price < adverse)) {
                adverse = price;
            }
        }
        return adverse;
    }
#ifndef PF_L1_HARVEST
    using Frame = NativeExecutionConsumer::CurrentExecutionFrame;
    static Frame parts(const NativeDecisionContext& decision, double price,
                       NativeCurrentQuoteKind kind, uint64_t origin, uint64_t cutoff) {
        return Frame(decision, price, kind, origin, cutoff);
    }
    static Frame from_point(const NativeCurrentPointView& point, uint64_t cutoff) {
        return Frame(point, cutoff);
    }
#endif
};

}  // inline namespace engine_script_run_v19
}  // namespace pineforge

using namespace pineforge;
namespace no = pineforge::native_order;
using Probe = NativeExecutionConsumerProbe;

namespace {

int passed = 0;
int failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            ++passed;                                                            \
        } else {                                                                 \
            ++failed;                                                            \
            std::fprintf(stderr, "CHECK FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }

constexpr std::int64_t kT0 = 1704067200000LL;  // 2024-01-01 00:00 UTC
constexpr std::int64_t kMinute = 60000;

struct Fnv {
    std::uint64_t h = 1469598103934665603ULL;
    void bytes(const void* p, std::size_t n) {
        const auto* c = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= c[i];
            h *= 1099511628211ULL;
        }
    }
    template <class T> void v(const T& x) { bytes(&x, sizeof x); }
    void s(const std::string& x) {
        v(x.size());
        bytes(x.data(), x.size());
    }
};

// ---- 1. Cooperative aborts at every boundary --------------------------------

enum Hook : int { Input, BarOpen, Bar_, Recalculate, Applied, Tick, HookCount };
constexpr const char* kHookNames[] = {"on_native_input", "on_native_bar_open", "on_native_bar",
                                      "on_native_recalculate", "on_native_applied",
                                      "on_native_tick"};

enum class Run : int { Batch, Fills, Stream, Ticks, Count };
constexpr const char* kRunNames[] = {"batch", "fills", "stream", "ticks"};

struct AbortHost final : NativeStrategyHost {
    Hook target = HookCount;
    int trigger = 0;
    int calls[HookCount] = {};
    int closes = 0;

    void poke(Hook hook) {
        if (++calls[hook] == trigger && hook == target) request_abort();
    }
    void on_native_input(const Bar&, const NativeInputContext&) override { poke(Input); }
    void on_native_bar_open(const Bar&, const NativeDecisionContext&) override { poke(BarOpen); }
    void on_native_recalculate(const Bar& bar, const NativeDecisionContext& ctx,
                               NativeCalculationReason why,
                               const no::ExecutionAppliedEvent* cause) override {
        poke(Recalculate);
        NativeStrategyHost::on_native_recalculate(bar, ctx, why, cause);
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        poke(Bar_);
        const int index = closes++;
        if (index % 2 == 0) (void)submit({no::Transact{1.0}, "l", ""});
        else (void)submit({no::Flatten{}, "x", ""});
    }
    void on_native_applied(const no::ExecutionAppliedEvent&,
                           const NativeDecisionContext&) override {
        poke(Applied);
    }
    void on_native_tick(const Bar&, const NativeTickContext&) override {
        poke(Tick);
        if (calls[Tick] % 3 == 1) (void)submit({no::Transact{1.0}, "t", ""});
    }
};

NativeRunSpec abort_spec(Run run, bool silent) {
    NativeRunSpec s;
    s.identity = {"perf-l1-point-checks", 1};
    s.input_tf = "1";
    s.script_tf = "1";
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
    s.fee_value = 0.0;
    if (run == Run::Fills) s.calculation = NativeCalculationTrigger::BarCloseAndFills;
    if (silent) s.abort_reporting = NativeAbortReporting::Quiet;
    return s;
}

std::vector<Bar> abort_bars(int n) {
    std::vector<Bar> bars;
    for (int i = 0; i < n; ++i) {
        const double p = 100.0 + (i % 5) - 2.0 * (i % 2);
        bars.push_back(Bar{p, p + 1.0, p - 1.0, p + 0.5, 1.0, kT0 + i * kMinute});
    }
    return bars;
}

// Every value a caught abort leaves behind, folded.
std::uint64_t abort_outcome(Run run, bool silent, Hook hook, int trigger) {
    AbortHost host;
    host.target = hook;
    host.trigger = trigger;
    Fnv f;
    if (host.configure_native(abort_spec(run, silent)).status != NativeSetupStatus::Applied)
        return 0;
    const auto bars = abort_bars(8);
    bool accepted = true;
    switch (run) {
    case Run::Batch:
    case Run::Fills:
        host.run(bars.data(), static_cast<int>(bars.size()));
        break;
    case Run::Stream:
        if (host.stream_begin(bars.data(), 3, "1", "1")) {
            for (int i = 3; i < 8 && accepted; ++i) accepted = host.stream_push_bar(bars[i]);
            (void)host.stream_end(false);
        }
        break;
    case Run::Ticks:
        if (host.stream_begin(bars.data(), 2, "1", "1")) {
            std::uint64_t sequence = 0;
            for (int k = 0; k < 10 && accepted; ++k) {
                accepted = host.stream_push_tick(
                    TradeTick{kT0 + 2 * kMinute + k * 20000, ++sequence, 101.0 + 0.1 * k, 1.0});
            }
            (void)host.stream_end(true);
        }
        break;
    case Run::Count:
        break;
    }
    const auto state = host.native_state();
    f.v(static_cast<int>(state.kind));
    f.v(static_cast<int>(state.failure.code));
    f.v(static_cast<int>(state.failure.operation));
    f.v(state.failure.ordinal);
    f.v(state.failure.discriminator);
    f.v(static_cast<int>(state.failure.context.kind));
    f.v(accepted);
    f.s(host.last_error());
    f.v(host.closed_trade_count());
    f.v(host.native_continuation_hash());
    for (int i = 0; i < HookCount; ++i) f.v(host.calls[i]);
    return f.h;
}

struct AbortPin {
    const char* name;
    std::uint64_t outcome;
};

// Harvested at f71cd820 (the lane's base): -DPF_L1_HARVEST, PF_POINT_CHECKS_DUMP=1.
// expectation corrected (96 values), because v19 folds the continuation over live state word-wise (native-consumer/v9) and each outcome folds native_continuation_hash() -- with that one read masked every outcome (lifecycle, failure code, operation, ordinal, discriminator, context kind, acceptance, error text, trade count, hook counts) is identical on 6976a808 and here; re-harvested the same way on INT19's tree; V19-A's tip (6211dc94) gives the same rows:
//   batch/error/on_native_input#1: outcome 1756094627297721218ULL -> 6393539083915214569ULL
//   batch/error/on_native_input#3: outcome 11369826695567426895ULL -> 12506965903089361792ULL
//   batch/error/on_native_bar_open#1: outcome 1756094627297721218ULL -> 6393539083915214569ULL
//   batch/error/on_native_bar_open#3: outcome 11369826695567426895ULL -> 12506965903089361792ULL
//   batch/error/on_native_bar#1: outcome 11856778920235710666ULL -> 2421246306453684171ULL
//   batch/error/on_native_bar#3: outcome 13682263432178950015ULL -> 1057123432612222053ULL
//   batch/error/on_native_recalculate#1: outcome 11856778920235710666ULL -> 2421246306453684171ULL
//   batch/error/on_native_recalculate#3: outcome 13682263432178950015ULL -> 1057123432612222053ULL
//   batch/error/on_native_applied#1: outcome 3755823117091528113ULL -> 5339722451315730454ULL
//   batch/error/on_native_applied#3: outcome 3748145109051711506ULL -> 1462432089140135432ULL
//   batch/error/on_native_tick#1: outcome 9490533002201463192ULL -> 14678299568377943283ULL
//   batch/error/on_native_tick#3: outcome 9490533002201463192ULL -> 14678299568377943283ULL
//   batch/quiet/on_native_input#1: outcome 7445641636614247716ULL -> 17464479845175602237ULL
//   batch/quiet/on_native_input#3: outcome 11765673491801525143ULL -> 2814996419086419291ULL
//   batch/quiet/on_native_bar_open#1: outcome 7445641636614247716ULL -> 17464479845175602237ULL
//   batch/quiet/on_native_bar_open#3: outcome 11765673491801525143ULL -> 2814996419086419291ULL
//   batch/quiet/on_native_bar#1: outcome 2499690141214795442ULL -> 8097870827278573096ULL
//   batch/quiet/on_native_bar#3: outcome 1699454288256336028ULL -> 8283080006278880170ULL
//   batch/quiet/on_native_recalculate#1: outcome 2499690141214795442ULL -> 8097870827278573096ULL
//   batch/quiet/on_native_recalculate#3: outcome 1699454288256336028ULL -> 8283080006278880170ULL
//   batch/quiet/on_native_applied#1: outcome 15528389346274352663ULL -> 7077975767914576205ULL
//   batch/quiet/on_native_applied#3: outcome 11565094891479924078ULL -> 18015243225726523608ULL
//   batch/quiet/on_native_tick#1: outcome 14776523166362922754ULL -> 18134814941465191729ULL
//   batch/quiet/on_native_tick#3: outcome 14776523166362922754ULL -> 18134814941465191729ULL
//   fills/error/on_native_input#1: outcome 118487784913891337ULL -> 17367621735129790840ULL
//   fills/error/on_native_input#3: outcome 10137690147833345466ULL -> 6438362476430832568ULL
//   fills/error/on_native_bar_open#1: outcome 118487784913891337ULL -> 17367621735129790840ULL
//   fills/error/on_native_bar_open#3: outcome 10137690147833345466ULL -> 6438362476430832568ULL
//   fills/error/on_native_bar#1: outcome 1495795819466735193ULL -> 8524839639049974501ULL
//   fills/error/on_native_bar#3: outcome 3287078954014157192ULL -> 1218036926535466279ULL
//   fills/error/on_native_recalculate#1: outcome 1495795819466735193ULL -> 8524839639049974501ULL
//   fills/error/on_native_recalculate#3: outcome 3287078954014157192ULL -> 1218036926535466279ULL
//   fills/error/on_native_applied#1: outcome 13619160401413074268ULL -> 16321821413986659647ULL
//   fills/error/on_native_applied#3: outcome 3672600079008313916ULL -> 4158348421283583503ULL
//   fills/error/on_native_tick#1: outcome 15212882780062550854ULL -> 7005226954935592032ULL
//   fills/error/on_native_tick#3: outcome 15212882780062550854ULL -> 7005226954935592032ULL
//   fills/quiet/on_native_input#1: outcome 8607891012036161609ULL -> 12196372944623838614ULL
//   fills/quiet/on_native_input#3: outcome 18067290218334227015ULL -> 17546638886976472941ULL
//   fills/quiet/on_native_bar_open#1: outcome 8607891012036161609ULL -> 12196372944623838614ULL
//   fills/quiet/on_native_bar_open#3: outcome 18067290218334227015ULL -> 17546638886976472941ULL
//   fills/quiet/on_native_bar#1: outcome 15300703146662902309ULL -> 14651796593170546901ULL
//   fills/quiet/on_native_bar#3: outcome 11807124725506397264ULL -> 7653501279252472751ULL
//   fills/quiet/on_native_recalculate#1: outcome 15300703146662902309ULL -> 14651796593170546901ULL
//   fills/quiet/on_native_recalculate#3: outcome 11807124725506397264ULL -> 7653501279252472751ULL
//   fills/quiet/on_native_applied#1: outcome 134644673297929323ULL -> 11885385292447102941ULL
//   fills/quiet/on_native_applied#3: outcome 649033997403019299ULL -> 2766548411790701755ULL
//   fills/quiet/on_native_tick#1: outcome 15705536381643470312ULL -> 9309647628106394874ULL
//   fills/quiet/on_native_tick#3: outcome 15705536381643470312ULL -> 9309647628106394874ULL
//   stream/error/on_native_input#1: outcome 1756094627297721218ULL -> 6393539083915214569ULL
//   stream/error/on_native_input#3: outcome 11369826695567426895ULL -> 12506965903089361792ULL
//   stream/error/on_native_bar_open#1: outcome 1756094627297721218ULL -> 6393539083915214569ULL
//   stream/error/on_native_bar_open#3: outcome 11369826695567426895ULL -> 12506965903089361792ULL
//   stream/error/on_native_bar#1: outcome 11856778920235710666ULL -> 2421246306453684171ULL
//   stream/error/on_native_bar#3: outcome 13682263432178950015ULL -> 1057123432612222053ULL
//   stream/error/on_native_recalculate#1: outcome 11856778920235710666ULL -> 2421246306453684171ULL
//   stream/error/on_native_recalculate#3: outcome 13682263432178950015ULL -> 1057123432612222053ULL
//   stream/error/on_native_applied#1: outcome 3755823117091528113ULL -> 5339722451315730454ULL
//   stream/error/on_native_applied#3: outcome 9325307024482353376ULL -> 1519443849186252878ULL
//   stream/error/on_native_tick#1: outcome 5260220416353173450ULL -> 3038203095228297971ULL
//   stream/error/on_native_tick#3: outcome 5260220416353173450ULL -> 3038203095228297971ULL
//   stream/quiet/on_native_input#1: outcome 7445641636614247716ULL -> 17464479845175602237ULL
//   stream/quiet/on_native_input#3: outcome 11765673491801525143ULL -> 2814996419086419291ULL
//   stream/quiet/on_native_bar_open#1: outcome 7445641636614247716ULL -> 17464479845175602237ULL
//   stream/quiet/on_native_bar_open#3: outcome 11765673491801525143ULL -> 2814996419086419291ULL
//   stream/quiet/on_native_bar#1: outcome 2499690141214795442ULL -> 8097870827278573096ULL
//   stream/quiet/on_native_bar#3: outcome 1699454288256336028ULL -> 8283080006278880170ULL
//   stream/quiet/on_native_recalculate#1: outcome 2499690141214795442ULL -> 8097870827278573096ULL
//   stream/quiet/on_native_recalculate#3: outcome 1699454288256336028ULL -> 8283080006278880170ULL
//   stream/quiet/on_native_applied#1: outcome 15528389346274352663ULL -> 7077975767914576205ULL
//   stream/quiet/on_native_applied#3: outcome 6155098625071516347ULL -> 7687428441803015905ULL
//   stream/quiet/on_native_tick#1: outcome 3723743665420506340ULL -> 17056314215193477229ULL
//   stream/quiet/on_native_tick#3: outcome 3723743665420506340ULL -> 17056314215193477229ULL
//   ticks/error/on_native_input#1: outcome 1756094627297721218ULL -> 6393539083915214569ULL
//   ticks/error/on_native_input#3: outcome 14829410450328021149ULL -> 12695806500583197548ULL
//   ticks/error/on_native_bar_open#1: outcome 1756094627297721218ULL -> 6393539083915214569ULL
//   ticks/error/on_native_bar_open#3: outcome 14829410450328021149ULL -> 12695806500583197548ULL
//   ticks/error/on_native_bar#1: outcome 11856778920235710666ULL -> 2421246306453684171ULL
//   ticks/error/on_native_bar#3: outcome 18336705861861220719ULL -> 17972753170949413387ULL
//   ticks/error/on_native_recalculate#1: outcome 11856778920235710666ULL -> 2421246306453684171ULL
//   ticks/error/on_native_recalculate#3: outcome 18336705861861220719ULL -> 17972753170949413387ULL
//   ticks/error/on_native_applied#1: outcome 3755823117091528113ULL -> 5339722451315730454ULL
//   ticks/error/on_native_applied#3: outcome 7763277276465876675ULL -> 2877234677515189003ULL
//   ticks/error/on_native_tick#1: outcome 6007603818950591425ULL -> 755879435947172223ULL
//   ticks/error/on_native_tick#3: outcome 7701426453337871836ULL -> 3586158540922747583ULL
//   ticks/quiet/on_native_input#1: outcome 7445641636614247716ULL -> 17464479845175602237ULL
//   ticks/quiet/on_native_input#3: outcome 2214991549771150920ULL -> 16192858612202111790ULL
//   ticks/quiet/on_native_bar_open#1: outcome 7445641636614247716ULL -> 17464479845175602237ULL
//   ticks/quiet/on_native_bar_open#3: outcome 2214991549771150920ULL -> 16192858612202111790ULL
//   ticks/quiet/on_native_bar#1: outcome 2499690141214795442ULL -> 8097870827278573096ULL
//   ticks/quiet/on_native_bar#3: outcome 1758333363839272318ULL -> 2497908218272532287ULL
//   ticks/quiet/on_native_recalculate#1: outcome 2499690141214795442ULL -> 8097870827278573096ULL
//   ticks/quiet/on_native_recalculate#3: outcome 1758333363839272318ULL -> 2497908218272532287ULL
//   ticks/quiet/on_native_applied#1: outcome 15528389346274352663ULL -> 7077975767914576205ULL
//   ticks/quiet/on_native_applied#3: outcome 355827270336707373ULL -> 17168120311484485675ULL
//   ticks/quiet/on_native_tick#1: outcome 8520651042860323138ULL -> 17346655304003294423ULL
//   ticks/quiet/on_native_tick#3: outcome 10915210028839708168ULL -> 9104465125362721709ULL
// K-IDX (Option A) re-pins the v19 continuation witnesses once: the
// script-space coordinate and named input coordinate are now folded.
constexpr AbortPin kAbortPins[] = {
        {"batch/error/on_native_input#1", 3560323808188698715ULL},
        {"batch/error/on_native_input#3", 3242237994065292507ULL},
        {"batch/error/on_native_bar_open#1", 3560323808188698715ULL},
        {"batch/error/on_native_bar_open#3", 3242237994065292507ULL},
        {"batch/error/on_native_bar#1", 18274505253103044355ULL},
        {"batch/error/on_native_bar#3", 7128076562107178007ULL},
        {"batch/error/on_native_recalculate#1", 18274505253103044355ULL},
        {"batch/error/on_native_recalculate#3", 7128076562107178007ULL},
        {"batch/error/on_native_applied#1", 5962112538319194182ULL},
        {"batch/error/on_native_applied#3", 16761785072693880059ULL},
        {"batch/error/on_native_tick#1", 1064083451052185069ULL},
        {"batch/error/on_native_tick#3", 1064083451052185069ULL},
        {"batch/quiet/on_native_input#1", 15855411169808214460ULL},
        {"batch/quiet/on_native_input#3", 18330722405539334964ULL},
        {"batch/quiet/on_native_bar_open#1", 15855411169808214460ULL},
        {"batch/quiet/on_native_bar_open#3", 18330722405539334964ULL},
        {"batch/quiet/on_native_bar#1", 15244034026730534949ULL},
        {"batch/quiet/on_native_bar#3", 15522275516133939991ULL},
        {"batch/quiet/on_native_recalculate#1", 15244034026730534949ULL},
        {"batch/quiet/on_native_recalculate#3", 15522275516133939991ULL},
        {"batch/quiet/on_native_applied#1", 6325881143001360144ULL},
        {"batch/quiet/on_native_applied#3", 927968700415268790ULL},
        {"batch/quiet/on_native_tick#1", 12893583462929514494ULL},
        {"batch/quiet/on_native_tick#3", 12893583462929514494ULL},
        {"fills/error/on_native_input#1", 9374070082406354326ULL},
        {"fills/error/on_native_input#3", 16134613282448265837ULL},
        {"fills/error/on_native_bar_open#1", 9374070082406354326ULL},
        {"fills/error/on_native_bar_open#3", 16134613282448265837ULL},
        {"fills/error/on_native_bar#1", 9660291061496025695ULL},
        {"fills/error/on_native_bar#3", 3200898139300934636ULL},
        {"fills/error/on_native_recalculate#1", 9660291061496025695ULL},
        {"fills/error/on_native_recalculate#3", 3200898139300934636ULL},
        {"fills/error/on_native_applied#1", 1873181469488193620ULL},
        {"fills/error/on_native_applied#3", 18364593085729179025ULL},
        {"fills/error/on_native_tick#1", 11599053408032152495ULL},
        {"fills/error/on_native_tick#3", 11599053408032152495ULL},
        {"fills/quiet/on_native_input#1", 512806339783699407ULL},
        {"fills/quiet/on_native_input#3", 14585734011108505323ULL},
        {"fills/quiet/on_native_bar_open#1", 512806339783699407ULL},
        {"fills/quiet/on_native_bar_open#3", 14585734011108505323ULL},
        {"fills/quiet/on_native_bar#1", 15746413390901049547ULL},
        {"fills/quiet/on_native_bar#3", 10179067546779602528ULL},
        {"fills/quiet/on_native_recalculate#1", 15746413390901049547ULL},
        {"fills/quiet/on_native_recalculate#3", 10179067546779602528ULL},
        {"fills/quiet/on_native_applied#1", 9247821118085001503ULL},
        {"fills/quiet/on_native_applied#3", 2125704302211299148ULL},
        {"fills/quiet/on_native_tick#1", 3967827899961809128ULL},
        {"fills/quiet/on_native_tick#3", 3967827899961809128ULL},
        {"stream/error/on_native_input#1", 3560323808188698715ULL},
        {"stream/error/on_native_input#3", 3242237994065292507ULL},
        {"stream/error/on_native_bar_open#1", 3560323808188698715ULL},
        {"stream/error/on_native_bar_open#3", 3242237994065292507ULL},
        {"stream/error/on_native_bar#1", 18274505253103044355ULL},
        {"stream/error/on_native_bar#3", 7128076562107178007ULL},
        {"stream/error/on_native_recalculate#1", 18274505253103044355ULL},
        {"stream/error/on_native_recalculate#3", 7128076562107178007ULL},
        {"stream/error/on_native_applied#1", 5962112538319194182ULL},
        {"stream/error/on_native_applied#3", 18140500761115835465ULL},
        {"stream/error/on_native_tick#1", 5948355266559318192ULL},
        {"stream/error/on_native_tick#3", 5948355266559318192ULL},
        {"stream/quiet/on_native_input#1", 15855411169808214460ULL},
        {"stream/quiet/on_native_input#3", 18330722405539334964ULL},
        {"stream/quiet/on_native_bar_open#1", 15855411169808214460ULL},
        {"stream/quiet/on_native_bar_open#3", 18330722405539334964ULL},
        {"stream/quiet/on_native_bar#1", 15244034026730534949ULL},
        {"stream/quiet/on_native_bar#3", 15522275516133939991ULL},
        {"stream/quiet/on_native_recalculate#1", 15244034026730534949ULL},
        {"stream/quiet/on_native_recalculate#3", 15522275516133939991ULL},
        {"stream/quiet/on_native_applied#1", 6325881143001360144ULL},
        {"stream/quiet/on_native_applied#3", 12530950112278122443ULL},
        {"stream/quiet/on_native_tick#1", 9176002025615057620ULL},
        {"stream/quiet/on_native_tick#3", 9176002025615057620ULL},
        {"ticks/error/on_native_input#1", 3560323808188698715ULL},
        {"ticks/error/on_native_input#3", 10706023567362351269ULL},
        {"ticks/error/on_native_bar_open#1", 3560323808188698715ULL},
        {"ticks/error/on_native_bar_open#3", 10706023567362351269ULL},
        {"ticks/error/on_native_bar#1", 18274505253103044355ULL},
        {"ticks/error/on_native_bar#3", 2851122174747762788ULL},
        {"ticks/error/on_native_recalculate#1", 18274505253103044355ULL},
        {"ticks/error/on_native_recalculate#3", 2851122174747762788ULL},
        {"ticks/error/on_native_applied#1", 5962112538319194182ULL},
        {"ticks/error/on_native_applied#3", 14982033343797752634ULL},
        {"ticks/error/on_native_tick#1", 6633416296699794458ULL},
        {"ticks/error/on_native_tick#3", 8401220712745923109ULL},
        {"ticks/quiet/on_native_input#1", 15855411169808214460ULL},
        {"ticks/quiet/on_native_input#3", 14133373906829835866ULL},
        {"ticks/quiet/on_native_bar_open#1", 15855411169808214460ULL},
        {"ticks/quiet/on_native_bar_open#3", 14133373906829835866ULL},
        {"ticks/quiet/on_native_bar#1", 15244034026730534949ULL},
        {"ticks/quiet/on_native_bar#3", 16656575874195564660ULL},
        {"ticks/quiet/on_native_recalculate#1", 15244034026730534949ULL},
        {"ticks/quiet/on_native_recalculate#3", 16656575874195564660ULL},
        {"ticks/quiet/on_native_applied#1", 6325881143001360144ULL},
        {"ticks/quiet/on_native_applied#3", 17359368186260815726ULL},
        {"ticks/quiet/on_native_tick#1", 17287525999126084928ULL},
        {"ticks/quiet/on_native_tick#3", 9804941054249243457ULL},
};

void aborts_are_caught_where_they_were() {
    const bool dump = std::getenv("PF_POINT_CHECKS_DUMP") != nullptr;
    std::size_t index = 0;
    int aborted = 0;
    for (int run = 0; run < static_cast<int>(Run::Count); ++run) {
        for (int silent = 0; silent < 2; ++silent) {
            for (int hook = 0; hook < HookCount; ++hook) {
                for (int trigger : {1, 3}) {
                    const std::string name = std::string(kRunNames[run]) + (silent ? "/quiet/" : "/error/")
                        + kHookNames[hook] + "#" + std::to_string(trigger);
                    const std::uint64_t outcome = abort_outcome(
                        static_cast<Run>(run), silent == 1, static_cast<Hook>(hook), trigger);
                    if (dump) {
                        std::printf("    {\"%s\", %lluULL},\n", name.c_str(),
                                    static_cast<unsigned long long>(outcome));
                        continue;
                    }
                    CHECK(index < sizeof(kAbortPins) / sizeof(kAbortPins[0]));
                    if (index >= sizeof(kAbortPins) / sizeof(kAbortPins[0])) return;
                    const AbortPin& pin = kAbortPins[index++];
                    const bool ok = name == pin.name && outcome == pin.outcome;
                    if (!ok) std::fprintf(stderr, "  abort outcome moved: %s\n", name.c_str());
                    CHECK(ok);
                    ++aborted;
                }
            }
        }
    }
    if (dump) return;
    CHECK(index == sizeof(kAbortPins) / sizeof(kAbortPins[0]));
    std::printf("  %d abort boundaries fail as they did\n", aborted);
}

// ---- 2. The margin sizing price ------------------------------------------------

struct SizingHost final : NativeStrategyHost {
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
};

void sizing_price_is_the_array_walk() {
    SizingHost host;
    NativeExecutionConsumer& c = host.consumer();
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double values[] = {101.0, 99.5, 100.0, 0.0, -3.0, inf, -inf, nan, 1e-300, 250.0};
    const NativePathPhase phases[] = {NativePathPhase::None, NativePathPhase::Open,
                                      NativePathPhase::High, NativePathPhase::Low,
                                      NativePathPhase::Close,
                                      static_cast<NativePathPhase>(7)};
    std::uint64_t compared = 0;
    const auto compare = [&](const Bar& bar) {
        for (int has_path = 0; has_path < 2; ++has_path) {
            for (int high_first = 0; high_first < 2; ++high_first) {
                Probe::set_margin_path(c, bar, high_first == 1, has_path == 1);
                for (int short_side = 0; short_side < 2; ++short_side) {
                    for (NativePathPhase phase : phases) {
                        for (double fallback : values) {
                            const double got = Probe::sizing(c, short_side == 1, phase, fallback);
                            const double want =
                                Probe::reference_sizing(c, short_side == 1, phase, fallback);
                            ++compared;
                            if (!same_bits(got, want)) {
                                ++failed;
                                std::fprintf(stderr, "  sizing price moved\n");
                                return;
                            }
                            ++passed;
                        }
                    }
                }
            }
        }
    };
    // Every assignment of the special values to the four waypoints.
    for (double open : values)
        for (double high : values)
            for (double low : values)
                for (double close : values) compare(Bar{open, high, low, close, 1.0, kT0});
    std::mt19937_64 rng(0x51A7E5ULL);
    std::uniform_real_distribution<double> price(50.0, 150.0);
    for (int i = 0; i < 2000; ++i) {
        const double o = price(rng), h = price(rng), l = price(rng), cl = price(rng);
        compare(Bar{o, std::max({o, h, l, cl}), std::min({o, h, l, cl}), cl, 1.0, kT0});
    }
    std::printf("  %llu sizing prices equal the array walk\n",
                static_cast<unsigned long long>(compared));
    CHECK(compared > 1000000);
}

// ---- 3. Frames from their parts ---------------------------------------------

#ifndef PF_L1_HARVEST
// Field by field: the coordinate has padding, which a byte compare would read.
bool same_coordinate(const NativeCoordinate& a, const NativeCoordinate& b) {
    return a.ordinal == b.ordinal && a.interval_index == b.interval_index
        && a.input_interval_index == b.input_interval_index
        && a.open_ms == b.open_ms && a.eligible_open_ms == b.eligible_open_ms
        && a.last_traded_close_ms == b.last_traded_close_ms
        && a.next_period_open_ms == b.next_period_open_ms
        && a.next_input_open_ms == b.next_input_open_ms
        && a.effective_time_ms == b.effective_time_ms
        && a.source_price_time_ms == b.source_price_time_ms
        && a.provenance == b.provenance && a.path_phase == b.path_phase
        && a.completion == b.completion;
}

bool same_frame(const Probe::Frame& a, const Probe::Frame& b) {
    const auto& x = a.point;
    const auto& y = b.point;
    const auto& d = x.decision;
    const auto& e = y.decision;
    return a.acceptance_cutoff == b.acceptance_cutoff && same_bits(x.price, y.price)
        && x.quote_kind == y.quote_kind && x.quote_origin_ordinal == y.quote_origin_ordinal
        && same_coordinate(d.coordinate, e.coordinate)
        && d.decision_floor_ms == e.decision_floor_ms
        && std::memcmp(&d.input_interval, &e.input_interval, sizeof d.input_interval) == 0
        && std::memcmp(&d.script_interval, &e.script_interval, sizeof d.script_interval) == 0
        && d.sub_index == e.sub_index && d.sub_count == e.sub_count
        && d.is_terminal_sub_bar == e.is_terminal_sub_bar && d.in_session == e.in_session
        && d.opens_session_day == e.opens_session_day
        && d.closes_session_day == e.closes_session_day
        && d.closes_session_day_open_ended == e.closes_session_day_open_ended
        && d.sub_bar_open_ms == e.sub_bar_open_ms && d.script_bar_open_ms == e.script_bar_open_ms
        && d.driver_statistics.intrabar_path_enabled == e.driver_statistics.intrabar_path_enabled
        && d.driver_statistics.sub_bars_per_script_bar
               == e.driver_statistics.sub_bars_per_script_bar
        && d.driver_statistics.samples_per_sub_bar == e.driver_statistics.samples_per_sub_bar
        && d.driver_statistics.sub_bars_processed == e.driver_statistics.sub_bars_processed
        && d.driver_statistics.sample_ticks_processed
               == e.driver_statistics.sample_ticks_processed;
}

void frames_are_the_aggregates() {
    std::mt19937_64 rng(0xF4A3E5ULL);
    int compared = 0;
    for (int i = 0; i < 20000; ++i) {
        NativeDecisionContext decision;
        decision.coordinate.open_ms = static_cast<std::int64_t>(rng());
        decision.coordinate.eligible_open_ms = static_cast<std::int64_t>(rng());
        decision.coordinate.last_traded_close_ms = static_cast<std::int64_t>(rng());
        decision.coordinate.next_period_open_ms = static_cast<std::int64_t>(rng());
        decision.coordinate.next_input_open_ms = static_cast<std::int64_t>(rng());
        decision.coordinate.effective_time_ms = static_cast<std::int64_t>(rng());
        decision.coordinate.source_price_time_ms = static_cast<std::int64_t>(rng());
        decision.coordinate.ordinal = rng();
        decision.coordinate.interval_index = static_cast<int>(rng() % 100000);
        decision.coordinate.input_interval_index = static_cast<int>(rng() % 100000);
        decision.coordinate.provenance = static_cast<NativePriceProvenance>(rng() % 8);
        decision.coordinate.path_phase = static_cast<NativePathPhase>(rng() % 5);
        decision.decision_floor_ms = static_cast<std::int64_t>(rng());
        decision.input_interval.open_ms = static_cast<std::int64_t>(rng());
        decision.script_interval.next_input_open_ms = static_cast<std::int64_t>(rng());
        decision.sub_index = static_cast<int>(rng() % 9);
        decision.sub_count = static_cast<int>(rng() % 9) + 1;
        decision.is_terminal_sub_bar = rng() % 2 == 0;
        decision.in_session = rng() % 2 == 0;
        decision.closes_session_day = rng() % 2 == 0;
        decision.sub_bar_open_ms = static_cast<std::int64_t>(rng());
        decision.script_bar_open_ms = static_cast<std::int64_t>(rng());
        decision.driver_statistics.sample_ticks_processed = rng();
        const double price = static_cast<double>(rng() % 100000) / 7.0;
        const auto kind = static_cast<NativeCurrentQuoteKind>(rng() % 2);
        const std::uint64_t origin = rng();
        const std::uint64_t cutoff = rng();
        // The aggregates the bar-open, tick and applied callbacks built.
        NativeCurrentPointView current;
        current.decision = decision;
        current.price = price;
        current.quote_kind = kind;
        current.quote_origin_ordinal = origin;
        CHECK(same_frame(Probe::parts(decision, price, kind, origin, cutoff),
                         Probe::Frame{current, cutoff}));
        CHECK(same_frame(Probe::from_point(current, cutoff), Probe::Frame{current, cutoff}));
        // The calculation callback's: the view's default quote kind and origin.
        NativeCurrentPointView calculation;
        calculation.decision = decision;
        calculation.price = price;
        CHECK(same_frame(Probe::parts(decision, price, NativeCurrentQuoteKind::MarketDecision,
                                      0, cutoff),
                         Probe::Frame{calculation, cutoff}));
        ++compared;
    }
    std::printf("  %d frames built from their parts are the aggregates\n", compared);
}
#endif

}  // namespace

int main() {
    aborts_are_caught_where_they_were();
    if (std::getenv("PF_POINT_CHECKS_DUMP") != nullptr) return 0;
    sizing_price_is_the_array_walk();
#ifndef PF_L1_HARVEST
    frames_are_the_aggregates();
#endif
    std::printf("test_native_point_checks: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
