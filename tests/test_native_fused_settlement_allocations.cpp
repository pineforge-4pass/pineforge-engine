// R5 lane PERF-L2: the fused settlement allocates nothing.
//
// A fill of the common case -- an opening from flat, the whole lot closing on
// the Book or on its opening's scope, a transaction or a ReverseTo through
// flat -- is inspected, projected, previewed and settled in one pass on the
// stack (BacktestEngine::NativeSettlementStage::OneLot,
// src/engine_execution.cpp). Before the lane each of those calls staged the
// settlement into a NativeSettlementStage whose vectors (the closing lots,
// their quantities, the quoted costs, the survivors) and whose close rows
// (NativeSettlementRows) were heap-allocated per call: 12 to 15 allocations
// per closing fill across inspect, preview and settle, and 3 per opening.
//
// This row counts every allocation (global_allocation_replacement.hpp) around
// each call on a warm engine -- its row, lot and stream vectors already hold
// room, the identifiers fit the string's inline buffer, and the precommit
// preview writes into a vector with room -- and requires zero for every call,
// fill shape, fee form and scope. It then reports, without a bound, the
// allocations per fill of two whole runs through the matcher (a market book
// and a bracket book), whose other per-fill work (plans, events, receipts)
// is not this lane's.
//
// Fail-before: the row compiles on the lane's base, where every call
// allocates (the counts are in the lane's report). Source-free, so it also
// runs in the kernel-only profile.
#include "global_allocation_replacement.hpp"

#include <pineforge/native_host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
namespace ex = pineforge::execution;
namespace no = pineforge::native_order;

int failures = 0;
int checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

class Warm final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}

    // A warm engine: room in every vector a settlement appends to.
    Warm(CommissionType fee, double value, bool stream) {
        commission_type_ = fee;
        commission_value_ = value;
        stream_observe_actions_ = stream;
        trades_.reserve(64);
        pyramid_entries_.reserve(8);
        stream_order_actions_.reserve(64);
        current_bar_.timestamp = 60000;
    }

    void hold(bool is_long, double qty) {
        PyramidEntry lot{100.0, 1000, qty, "E", 0};
        lot.entry_incarnation = 7;
        lot.entry_commission_account = 0.1 * qty;
        pyramid_entries_.clear();
        pyramid_entries_.push_back(lot);
        position_side_ = is_long ? PositionSide::LONG : PositionSide::SHORT;
        position_qty_ = qty;
        position_entry_price_ = 100.0;
        position_entry_count_ = 1;
        position_cycle_seq_ = 4;
        next_position_cycle_seq_ = 5;
    }

    void flat() {
        pyramid_entries_.clear();
        position_side_ = PositionSide::FLAT;
        position_qty_ = 0.0;
        position_entry_count_ = 0;
        position_cycle_seq_ = 0;
    }

    struct Counts {
        std::size_t inspect = 0;
        std::size_t project = 0;
        std::size_t preview = 0;
        std::size_t settle = 0;
        ex::Status status = ex::Status::NoEffect;
    };

    // One fill through the four entries in the consumer's order, counting
    // each call's allocations.
    Counts fill(const std::optional<ex::Action>& action, double reverse_units,
                const ex::CloseScope& scope) {
        Counts counts;
        ex::Fill fill{120.0, "X", "c", 100};
        ex::PhysicalExecutionContext context;
        context.effective_time_ms = 120000;
        context.interval_index = 3;
        const ex::ReverseTo reversal{reverse_units};

        std::size_t before = global_allocation::allocations;
        const auto inspection = action ? inspect_native_settlement_scoped(*action, fill, scope)
                                       : inspect_native_reversal_v1(reversal, fill);
        counts.inspect = global_allocation::allocations - before;

        before = global_allocation::allocations;
        const auto projection = action ? project_native_settlement_scoped_v1(*action, fill, scope)
                                       : project_native_reversal_v1(reversal, fill);
        counts.project = global_allocation::allocations - before;
        CHECK(projection.status == ex::Status::Applied);

        fill.commission_account = inspection.current_ticket;
        before = global_allocation::allocations;
        const auto readiness = action
            ? preview_native_settlement_commit(*action, fill, context, scope, nullptr, account_,
                                               row_pnl_)
            : preview_native_settlement_commit(reversal, fill, context, account_, row_pnl_);
        counts.preview = global_allocation::allocations - before;
        CHECK(readiness == ex::Status::Applied);

        before = global_allocation::allocations;
        const auto result = action ? settle_native_execution_scoped_at(*action, fill, context, scope)
                                   : settle_native_reversal_at_v1(reversal, fill, context);
        counts.settle = global_allocation::allocations - before;
        counts.status = result.status;
        return counts;
    }

private:
    ex::AccountEffectProjection account_;
    std::vector<double> row_pnl_ = std::vector<double>(4);
};

void direct_calls() {
    struct Shape {
        const char* name;
        bool held;
        bool is_long;
        std::optional<ex::Action> action;
        double reverse_units;
        bool opening_scope;
    };
    const Shape shapes[] = {
        {"open long from flat", false, true, ex::Action{order_action::Transact{2.0}}, 0.0, false},
        {"open short from flat", false, true, ex::Action{order_action::Transact{-2.0}}, 0.0, false},
        {"flatten the lot", true, true, ex::Action{ex::Flatten{}}, 0.0, false},
        {"reduce the whole lot", true, false, ex::Action{order_action::Reduce{2.0}}, 0.0, false},
        {"reduce on the opening's scope", true, true, ex::Action{order_action::Reduce{2.0}}, 0.0, true},
        {"transact to flat", true, true, ex::Action{order_action::Transact{-2.0}}, 0.0, false},
        {"transact through flat", true, false, ex::Action{order_action::Transact{3.0}}, 0.0, false},
        {"reverse to a target", true, true, std::nullopt, -1.5, false},
    };
    const CommissionType fees[] = {CommissionType::PERCENT, CommissionType::CASH_PER_ORDER,
                                   CommissionType::CASH_PER_CONTRACT};
    int calls = 0;
    std::size_t total = 0;
    for (const auto fee : fees) {
        for (const bool stream : {false, true}) {
            Warm engine(fee, fee == CommissionType::PERCENT ? 0.1 : 1.5, stream);
            for (const auto& shape : shapes) {
                if (shape.held) engine.hold(shape.is_long, 2.0);
                else engine.flat();
                const ex::CloseScope scope = shape.opening_scope
                    ? ex::CloseScope{ex::OpeningExposure{7, 4}} : ex::CloseScope{ex::Book{}};
                const auto counts = engine.fill(shape.action, shape.reverse_units, scope);
                CHECK(counts.status == ex::Status::Applied);
                const std::size_t sum = counts.inspect + counts.project + counts.preview
                    + counts.settle;
                if (sum != 0) {
                    std::printf("  %s (fee %d, stream %d): inspect %zu, project %zu, preview %zu, "
                                "settle %zu allocations\n", shape.name, static_cast<int>(fee),
                                stream ? 1 : 0, counts.inspect, counts.project, counts.preview,
                                counts.settle);
                }
                CHECK(counts.inspect == 0);
                CHECK(counts.project == 0);
                CHECK(counts.preview == 0);
                CHECK(counts.settle == 0);
                calls += 4;
                total += sum;
            }
        }
    }
    std::printf("direct calls: %d settlement calls on warm engines, %zu allocations\n", calls,
                total);
}

// PERF0-K's common-case books, run through the matcher: a market entry and a
// flatten every fifth bar, and a market entry whose fill arms a take-profit
// and a stop-loss on its own lot in one OCA group.
class Book final : public NativeStrategyHost {
public:
    explicit Book(bool bracket) : bracket_(bracket) {}
    long fills = 0;
    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        ++bars_;
        const double position = physical_position().signed_units;
        if (!bracket_) {
            if (bars_ % 5 == 0) {
                no::Request request;
                if (position == 0.0) request.intent = no::Transact{1.0};
                else request.intent = no::Flatten{};
                request.label = "m";
                (void)submit_market(request);
            }
            return;
        }
        if (position != 0.0 || bars_ % 5 != 0) return;
        const auto parent = submit(no::Request{no::Transact{1.0}, "E", ""});
        if (!parent.handle) return;
        no::WaitForApplied wait;
        wait.parent = *parent.handle;
        no::Member member;
        member.group = static_cast<std::uint64_t>(bars_);
        no::Request take{no::Reduce{no::ExplicitUnits{1.0}}, "x", ""};
        take.trigger = no::Limit{bar.close * 1.004};
        take.owner = wait;
        take.group = member;
        no::Request stop{no::Reduce{no::ExplicitUnits{1.0}}, "x", ""};
        stop.trigger = no::Stop{bar.close * 0.996};
        stop.owner = wait;
        stop.group = member;
        (void)submit(take);
        (void)submit(stop);
    }
    void on_native_applied(const no::ExecutionAppliedEvent&, const NativeDecisionContext&) override {
        ++fills;
    }

private:
    bool bracket_;
    long bars_ = 0;
};

std::vector<Bar> tape(int count) {
    std::vector<Bar> bars;
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    double price = 100.0;
    for (int index = 0; index < count; ++index) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        const double move = static_cast<double>(static_cast<int>(state % 9) - 4) * 0.25;
        const double open = price;
        const double close = price + move;
        Bar bar{};
        bar.open = open;
        bar.close = close;
        bar.high = (open > close ? open : close) + 0.25;
        bar.low = (open < close ? open : close) - 0.25;
        bar.volume = 1.0;
        bar.timestamp = 1736121600000LL + static_cast<std::int64_t>(index) * 300000;
        bars.push_back(bar);
        price = close;
    }
    return bars;
}

void matcher_runs() {
    for (const bool bracket : {false, true}) {
        NativeRunSpec spec;
        spec.identity = {"l2-alloc", 1};
        spec.input_tf = "5";
        spec.script_tf = "5";
        spec.tickerid = "TEST:L2ALLOC";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
        spec.initial_capital = 1.0e6;
        spec.point_value = 1.0;
        spec.account_fx = 1.0;
        spec.price_tick = 0.25;
        spec.fee_kind = NativeFeeKind::Percent;
        spec.fee_value = 0.05;
        const auto bars = tape(2000);
        Book host(bracket);
        CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
        const std::size_t before = global_allocation::allocations;
        host.run(bars.data(), static_cast<int>(bars.size()));
        const std::size_t allocations = global_allocation::allocations - before;
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        CHECK(host.fills > 100);
        std::printf("%s book: %ld fills, %zu allocations in the run, %.2f per fill "
                    "(reported, not bounded)\n", bracket ? "bracket" : "market", host.fills,
                    allocations, host.fills ? static_cast<double>(allocations) / host.fills : 0.0);
    }
}

}  // namespace

int main() {
    direct_calls();
    matcher_runs();
    std::printf("%d checks\n", checks);
    if (failures != 0) {
        std::printf("test_native_fused_settlement_allocations: %d of %d checks failed\n",
                    failures, checks);
        return 1;
    }
    std::printf("test_native_fused_settlement_allocations: ok\n");
    return 0;
}
