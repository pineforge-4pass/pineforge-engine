/*
 * test_native_current_execution_rate.cpp -- R5 lane B-ADAPTER, item 5
 * (R5 lane F3's finding 2).
 *
 * A current execution (NativeStrategyHost::execute_current) settles at its own
 * cursor: its terms, inspection, admission, preview and settlement convert at
 * that instant's account-currency rate. Until this lane the kernel got there
 * by presenting the cursor as the engine's bar clock --
 * NativeExecutionConsumer::consume_matched_request wrote
 * current_bar_.timestamp -- and R5 lane F3 made execute_current save the
 * frame's clock around the call and restore it, so the host got its clock
 * back afterwards. E21's ruling for a host command is to thread the rate and
 * never write the clock, which F3 could not do without opening
 * engine_execution.cpp. Now the rate is threaded: the execution's cursor rate
 * rides the terms facts (active_fx), the inspection
 * (inspect_native_*_at), the preview and settlement contexts
 * (PhysicalExecutionContext::account_fx, from which the one-lot and staged
 * settlements take their quote rate and build their rows), the account
 * observation and the risk note; a driver point's matching still presents
 * its own instant, and a current execution presents nothing.
 *
 * 1. In a bar-open frame (the clock at the bar's open) the command executes
 *    at the decision floor, a bar later, across an FX step. The execution's
 *    own terms hook now sees the frame's clock with the execution's rate in
 *    its facts (before: the cursor's clock); the new lot's fee is taken at
 *    the stepped rate and the host's clock, rate and marked equity after the
 *    call are the frame's -- both as before.
 * 2. A current close in the same frame books its row at the execution's rate:
 *    P&L, fees and the excursion columns, the same numbers the presented
 *    clock gave.
 * 3. The kernel sources: execute_current names no clock;
 *    consume_matched_request writes it only when it is matching a driver
 *    point; the candidate inspection and the sized-basis resolution read no
 *    clock; the settle and preview entries take their rate from the context.
 * The byte-identity of the whole change is the pinned rows of every other
 * FX suite (test_native_margin_fx_clock, test_native_bare_host_contracts,
 * test_affordability_fx, test_adapter_range_end_fx) and the corpus.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <pineforge/bar.hpp>
#include <pineforge/native_fx_curve.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/native_order.hpp>
#include <pineforge/native_run_spec.hpp>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        ++checks;                                                              \
        if (!(expr)) {                                                         \
            ++failures;                                                        \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
        }                                                                      \
    } while (0)

constexpr std::int64_t kT0 = 1735689600000LL;  // 2025-01-01 00:00 UTC
constexpr std::int64_t kMinute = 60000LL;

NativeRunSpec spec_for(const char* key) {
    NativeRunSpec spec;
    spec.identity.session_key = key;
    spec.identity.run_number = 1;
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 1000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::Percent;
    spec.fee_value = 0.1;
    return spec;
}

// Bars 0-1 at 100, from bar 2 at 110; the rate steps 1.0 -> 2.5 at +3 min.
std::vector<Bar> tape() {
    std::vector<Bar> bars;
    for (int i = 0; i < 5; ++i) {
        const double p = i < 2 ? 100.0 : 110.0;
        bars.push_back(Bar{p, p, p, p, 1.0, kT0 + i * kMinute});
    }
    return bars;
}

// Buys 15 on bar 0's calculation; in bar 2's bar-open frame, submits one
// more unit (or a flatten) and executes it at once.
struct Host final : NativeStrategyHost {
    bool flatten = false;
    int bars = 0;
    int opens = 0;
    bool in_command = false;
    std::int64_t clock_before = 0;
    std::int64_t clock_after = 0;
    double fx_before = 0.0;
    double fx_after = 0.0;
    double equity_before = 0.0;
    double equity_after = 0.0;
    std::int64_t executed_at = 0;
    bool executed = false;
    // What the execution's own terms hook saw.
    int hook_calls = 0;
    std::int64_t hook_clock = 0;
    double hook_fx = 0.0;
    std::int64_t hook_fx_instant = 0;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (bars++ == 0) (void)submit({no::Transact{15.0}, "entry", ""});
    }
    void on_native_bar_open(const Bar& bar, const NativeDecisionContext&) override {
        if (opens++ != 2) return;
        clock_before = current_bar_.timestamp;
        fx_before = active_account_currency_fx();
        equity_before = marked_equity(bar.open);
        no::Request request;
        if (flatten) request.intent = no::Flatten{};
        else request.intent = no::Transact{1.0};
        request.label = flatten ? "flat" : "mkt";
        const auto placed = submit(request);
        if (!placed.handle) return;
        NativeCurrentExecution command;
        command.target = *placed.handle;
        in_command = true;
        const auto result = execute_current(command);
        in_command = false;
        if (const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&result)) {
            executed = true;
            executed_at = applied->cursor.point.effective_time_ms;
        }
        clock_after = current_bar_.timestamp;
        fx_after = active_account_currency_fx();
        equity_after = marked_equity(bar.open);
    }
    no::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const override {
        if (in_command) {
            auto* self = const_cast<Host*>(this);
            ++self->hook_calls;
            self->hook_clock = current_bar_.timestamp;
            self->hook_fx = facts.active_fx;
            self->hook_fx_instant = facts.fx_effective_time_ms;
        }
        return no::ExecutionTerms{facts.default_resolved_price, std::nullopt,
                                  no::OpeningShape::Transact};
    }
    using BacktestEngine::trades_;
};

void run(Host& host, const char* key) {
    CHECK(host.configure_native(spec_for(key)).status == NativeSetupStatus::Applied);
    CHECK(host.configure_native_fx_curve(NativeFxCurve{{kT0 + 3 * kMinute}, {2.5}}).status
          == NativeSetupStatus::Applied);
    const auto bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
}

void the_execution_converts_at_its_cursor_and_presents_nothing() {
    Host host;
    run(host, "b-adapter-rate-open");
    CHECK(host.executed);
    std::printf("  bar-open command: clock %+lld min, executed at %+lld min; inside the"
                " terms hook: clock %+lld min, facts rate %.2f at %+lld min; after:"
                " clock %+lld min, rate %.2f, marked equity(110) %.6f -> %.6f\n",
                static_cast<long long>((host.clock_before - kT0) / kMinute),
                static_cast<long long>((host.executed_at - kT0) / kMinute),
                static_cast<long long>((host.hook_clock - kT0) / kMinute), host.hook_fx,
                static_cast<long long>((host.hook_fx_instant - kT0) / kMinute),
                static_cast<long long>((host.clock_after - kT0) / kMinute), host.fx_after,
                host.equity_before, host.equity_after);
    CHECK(host.clock_before == kT0 + 2 * kMinute);
    CHECK(host.executed_at == kT0 + 3 * kMinute);
    // The execution's own hook: the frame's clock, the execution's rate.
    CHECK(host.hook_calls >= 1);
    CHECK(host.hook_clock == kT0 + 2 * kMinute);
    CHECK(host.hook_fx == 2.5);
    CHECK(host.hook_fx_instant == kT0 + 3 * kMinute);
    // After the call: the frame's clock, rate and marked equity, less the new
    // unit's fee -- 0.1 % of 110 at the stepped rate, 0.11 x 2.5.
    CHECK(host.clock_after == host.clock_before);
    CHECK(host.fx_after == 1.0);
    CHECK(std::abs(host.equity_before - 1148.5) < 1e-9);
    bool found = false;
    for (const NativeOpenLot& lot : host.native_open_lots(110.0)) {
        if (lot.entry_label != "mkt") continue;
        found = true;
        CHECK(lot.entry_time_ms == kT0 + 3 * kMinute);
        CHECK(std::abs(lot.entry_commission - 0.275) < 1e-12);
        CHECK(std::abs(host.equity_after - (host.equity_before - lot.entry_commission)) < 1e-9);
    }
    CHECK(found);
}

void a_current_close_books_its_row_at_the_execution_rate() {
    Host host;
    host.flatten = true;
    run(host, "b-adapter-rate-close");
    CHECK(host.executed);
    CHECK(host.trades_.size() == 1);
    if (host.trades_.size() != 1) return;
    const Trade& row = host.trades_[0];
    // 15 bought at 100 (fee 1.5 at 1.0), sold at 110 at +3 min at 2.5:
    // P&L (110 - 100) x 15 x 2.5 = 375, less 1.5 and the exit fee
    // 110 x 15 x 2.5 x 0.1 % = 4.125.
    const double exit_fee = 110.0 * 15.0 * 1.0 * 2.5 * (0.1 / 100.0);
    const double pnl = (110.0 - 100.0) * 15.0 * 1.0 * 2.5 - (1.5 + exit_fee);
    std::printf("  current close: exit at %+lld min @%.2f, pnl %.9f (want %.9f), commission"
                " %.9f (want %.9f), runup %.9f\n",
                static_cast<long long>((row.exit_time - kT0) / kMinute), row.exit_price, row.pnl,
                pnl, row.commission, 1.5 + exit_fee, row.max_runup);
    CHECK(row.exit_time == kT0 + 3 * kMinute);
    CHECK(row.exit_price == 110.0);
    CHECK(std::abs(row.pnl - pnl) < 1e-9);
    CHECK(std::abs(row.commission - (1.5 + exit_fee)) < 1e-12);
    CHECK(std::abs(row.pnl_pct - pnl / (100.0 * 15.0 * 2.5) * 100.0) < 1e-9);
    // The favorable excursion is the move to the fill, at the row's rate, less
    // the entry fee: 150 x 2.5 - 1.5.
    CHECK(std::abs(row.max_runup - (150.0 * 2.5 - 1.5)) < 1e-9);
    CHECK(host.clock_after == host.clock_before);
}

#ifdef PINEFORGE_B_ADAPTER_KERNEL_FILES
std::string read_text(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string squeezed(const std::string& text) {
    std::string out;
    for (const char c : text)
        if (c != ' ' && c != '\n' && c != '\t' && c != '\r') out.push_back(c);
    return out;
}

// (signature, body) of every out-of-line definition starting at `head`.
std::vector<std::pair<std::string, std::string>> definitions_of(const std::string& code,
                                                                 const std::string& head) {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto at = code.find(head); at != std::string::npos; at = code.find(head, at + 1)) {
        const auto open = code.find('{', at);
        if (open == std::string::npos || code.find(';', at) < open) continue;
        int depth = 0;
        for (auto i = open; i < code.size(); ++i) {
            if (code[i] == '{') ++depth;
            if (code[i] == '}' && --depth == 0) {
                out.emplace_back(code.substr(at, open - at), code.substr(open + 1, i - open - 1));
                break;
            }
        }
    }
    return out;
}
#endif

void the_kernel_threads_the_rate() {
#ifdef PINEFORGE_B_ADAPTER_KERNEL_FILES
    std::string engine_execution, consumer;
    const std::string joined = PINEFORGE_B_ADAPTER_KERNEL_FILES;
    for (std::size_t start = 0; start <= joined.size();) {
        const auto bar = joined.find('|', start);
        const auto path = joined.substr(start, bar == std::string::npos ? std::string::npos
                                                                        : bar - start);
        const auto ends = [&](const char* tail) {
            const std::string t = tail;
            return path.size() >= t.size() && path.compare(path.size() - t.size(), t.size(), t) == 0;
        };
        if (ends("/src/engine_execution.cpp")) engine_execution = squeezed(read_text(path));
        if (ends("/src/native_execution_consumer.cpp")) consumer = squeezed(read_text(path));
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    CHECK(!engine_execution.empty());
    CHECK(!consumer.empty());
    if (engine_execution.empty() || consumer.empty()) return;

    // execute_current names no clock.
    const auto execute = definitions_of(consumer, "NativeExecutionConsumer::execute_current(");
    CHECK(execute.size() == 1);
    if (execute.size() == 1) {
        const bool names = execute[0].second.find("current_bar_") != std::string::npos;
        if (names) std::printf("  execute_current names the bar clock\n");
        CHECK(!names);
    }
    // consume_matched_request writes the clock once, only when matching.
    const auto consume =
        definitions_of(consumer, "NativeExecutionConsumer::consume_matched_request(");
    CHECK(consume.size() == 1);
    if (consume.size() == 1) {
        const std::string& body = consume[0].second;
        std::size_t writes = 0;
        for (auto at = body.find("current_bar_.timestamp="); at != std::string::npos;
             at = body.find("current_bar_.timestamp=", at + 1)) {
            ++writes;
        }
        const bool guarded =
            body.find("if(!current)engine.current_bar_.timestamp=") != std::string::npos;
        if (writes != 1 || !guarded) {
            std::printf("  consume_matched_request writes the clock %zu time(s)%s\n", writes,
                        guarded ? "" : ", unguarded");
        }
        CHECK(writes == 1);
        CHECK(guarded);
    }
    // The candidate inspection and the sized basis read no clock.
    for (const char* head : {"NativeExecutionConsumer::inspect_candidate(",
                             "NativeExecutionConsumer::resolve_sized_units("}) {
        const auto found = definitions_of(consumer, head);
        CHECK(found.size() == 1);
        if (found.size() != 1) continue;
        const bool clock = found[0].second.find("active_account_currency_fx") != std::string::npos
            || found[0].second.find("marked_equity(") != std::string::npos
            || found[0].second.find("marked(engine") != std::string::npos
            || found[0].second.find("current_bar_") != std::string::npos;
        if (clock) std::printf("  %s reads a clock\n", head);
        CHECK(!clock);
    }
    // The settle and preview entries take their rate from the context.
    for (const char* head : {"BacktestEngine::settle_with_membership(",
                             "BacktestEngine::settle_native_reversal_at_v1(",
                             "BacktestEngine::NativeSettlementStage::OneLot::preview_keeping("}) {
        const auto found = definitions_of(engine_execution, head);
        CHECK(!found.empty());
        for (const auto& definition : found) {
            const bool takes = definition.second.find("context.account_fx?*context.account_fx")
                != std::string::npos;
            if (!takes) std::printf("  %s does not take the context's rate\n", head);
            CHECK(takes);
        }
    }
#else
    std::printf("  PINEFORGE_B_ADAPTER_KERNEL_FILES undefined\n");
    CHECK(false);
#endif
}

void test(const char* name, void (*fn)()) {
    const int before = failures;
    std::printf("-- %s\n", name);
    fn();
    if (failures != before) std::printf("   ^^ %d failure(s)\n", failures - before);
}

}  // namespace

int main() {
    test("a current execution converts at its cursor and presents no clock",
         the_execution_converts_at_its_cursor_and_presents_nothing);
    test("a current close books its row at the execution's rate",
         a_current_close_books_its_row_at_the_execution_rate);
    test("the kernel threads the rate", the_kernel_threads_the_rate);
    std::printf("R5 B-ADAPTER current execution rate: %d checks, %d failures\n", checks,
                failures);
    return failures ? 1 : 0;
}
