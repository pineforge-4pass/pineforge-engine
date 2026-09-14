// Literal native event/cursor tests. No Pine source, corpus, or external tape.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/pending_order_mirror.hpp>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <vector>
#include <cstddef>
#include "fixtures/pending_order_prefix/c45-v1.hpp"
#define PF_PREFIX_FIELD(name) \
    static_assert(offsetof(pf_pending_order_v1_t, name) == offsetof(c45_pending_order_t, name), "v1 prefix offset changed"); \
    static_assert(sizeof(((pf_pending_order_v1_t*)0)->name) == sizeof(((c45_pending_order_t*)0)->name), "v1 prefix field size changed");
#include "fixtures/pending_order_prefix/c45-fields.inc"
#undef PF_PREFIX_FIELD
static_assert(offsetof(pf_pending_order_v1_t, birth_cause) >= sizeof(c45_pending_order_t), "new facts must append after the v1 prefix");
using namespace pineforge;
using pineforge::source::PendingOrder;
namespace pineforge {
void fill_pending_order_mirror(const source::PendingOrder&, pf_pending_order_v1_t*);
}
namespace {
int failed = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); ++failed; } } while (0)
const double nan = std::numeric_limits<double>::quiet_NaN();
const Bar bars[] = {{100, 101, 99, 100, 1, 0}, {100, 110, 95, 108, 1, 60000}};

class Probe : public pineforge::source::PineStrategyHost {
public:
    Probe() {
        initial_capital_ = 100000;
        calc_on_order_fills_ = true;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1;
        pyramiding_ = 10;
        commission_value_ = 0;
        syminfo_mintick_ = 0.01;
    }
    const PendingOrder& get(const std::string& id) const {
        for (const auto& o : pending_orders_) if (o.id == id) return o;
        throw std::runtime_error("missing test order " + id);
    }
    void direct(const std::string& id) { strategy_entry(id, true, 1, nan, 1); }
    void set_birth(const std::string& id, const OrderBirth& birth) {
        for (auto& o : pending_orders_) if (o.id == id) { o.birth = birth; return; }
        throw std::runtime_error("missing mutation target");
    }
    void clear_trailing_trigger(const std::string& id) {
        for (auto& o : pending_orders_) if (o.id == id) {
            o.legs.set_trail_points(o.legs.set_trail_price(nan));
            return;
        }
    }
    std::size_t pending_count() const { return pending_orders_.size(); }
    const std::vector<uint64_t>& recorded_hashes() const { return broker_state_hashes_; }
};

// The first callback is triggered by fill1. It directly closes that lot,
// advancing the broker to fill2, then emits another order in the SAME callback.
// That order must still name fill1, while the next callback names fill2.
class DirectCascade : public Probe {
public:
    int bar_one_calls = 0;
    std::vector<OrderBirth> observed;
    OrderBirth after_direct_fill;
    OrderBirth cloned_command;
    OrderBirth replaced_birth, replacement_birth;
    uint64_t replaced_incarnation = 0, replacement_incarnation = 0;
    int64_t replaced_priority = 0, replacement_priority = 0;
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("seed", true, nan, nan, 1);
            direct("replace");
            return;
        }
        if (bar_index_ != 1) return;
        const int call = bar_one_calls++;
        direct("witness-" + std::to_string(call));
        observed.push_back(get("witness-" + std::to_string(call)).birth);
        if (call == 0) {
            replaced_birth = get("replace").birth;
            replaced_incarnation = get("replace").incarnation;
            replaced_priority = get("replace").created_seq;
            direct("replace");
            replacement_birth = get("replace").birth;
            replacement_incarnation = get("replace").incarnation;
            replacement_priority = get("replace").created_seq;
            strategy_close("seed", "", nan, nan, true);
            direct("after-direct");
            after_direct_fill = get("after-direct").birth;
            DirectCascade copy(*this);
            copy.direct("clone-command");
            cloned_command = copy.get("clone-command").birth;
        }
    }
};

class LaterOpenPolicy : public Probe {
public:
    int bar_one_calls = 0;
    OrderBirth trailing_birth, priced_birth;
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) {
            strategy_entry("A", true, nan, nan, 1);
            strategy_entry("B", true, nan, nan, 1);
            return;
        }
        if (bar_index_ != 1 || bar_one_calls++ != 1) return;
        strategy_exit("trailing", "A", nan, nan, 100000, 1);
        strategy_exit("priced", "B", nan, 1);
        trailing_birth = get("trailing").birth;
        priced_birth = get("priced").birth;
    }
};

class SegmentOrigin : public Probe {
public:
    int bar_one_calls = 0;
    OrderBirth receipt;
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) { strategy_entry("stop", true, nan, 105, 1); return; }
        if (bar_index_ == 1 && bar_one_calls++ == 0) {
            direct("segment-witness");
            receipt = get("segment-witness").birth;
        }
    }
};

class ProducerOrigins : public Probe {
public:
    bool captured = false;
    std::vector<OrderBirth> births;
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) { strategy_entry("seed", true, nan, nan, 1); return; }
        if (bar_index_ != 1 || captured) return;
        captured = true;
        strategy_entry("entry", true, 1, nan, 1);
        strategy_order("raw", true, 1, 1);
        strategy_exit("exit", "seed", nan, 1);
        for (const std::string id : {"entry", "raw", "exit"}) births.push_back(get(id).birth);
    }
};

class ThrowsInFill : public Probe {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("seed", true, nan, nan, 1);
        else throw std::runtime_error("literal callback failure");
    }
};

void rejects(const std::function<void()>& f) {
    bool rejected = false;
    try { f(); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}

void value_contract() {
    const auto open = BirthCursor::point(BirthCursorDomain::HistoricalPath, 0, 4);
    const auto close = BirthCursor::point(BirthCursorDomain::HistoricalPath, 3, 4);
    const auto first = OrderBirth::fill_evaluation(2, 120000, open, 100, 7, 7, 1);
    const auto later = OrderBirth::fill_evaluation(2, 120000, open, 100, 8, 8, 2);
    const auto terminal = OrderBirth::fill_evaluation(2, 120000, close, 100, 9, 9, 3);
    CHECK(first.from_fill() && !first.at_terminal_fill());
    CHECK(compat::pine::first_open_fill_evaluation(first));
    CHECK(!compat::pine::first_open_fill_evaluation(later));
    CHECK(terminal.at_terminal_fill() && !terminal.cursor().first_point());
    CHECK(terminal.cursor().following_segment() == -1);
    // Equal prices do not collapse distinct physical path positions.
    CHECK(first.cursor_price() == terminal.cursor_price());
    CHECK(first.cursor().index() != terminal.cursor().index());
    const auto batch = OrderBirth::fill_evaluation(2, 120000, open, 100, 10, 12, 1);
    CHECK(batch.first_fill() == 10 && batch.last_fill() == 12);
    const auto copy = batch;
    CHECK(copy.first_fill() == 10 && copy.last_fill() == 12);
    rejects([&] { OrderBirth::fill_evaluation(2, 0, open, 100, 0, 1, 1); });
    rejects([&] { OrderBirth::fill_evaluation(2, 0, open, 100, 3, 2, 1); });
    rejects([&] { OrderBirth::fill_evaluation(2, 0, open, 100, 1, 1, 0); });
    rejects([&] { OrderBirth::fill_evaluation(2, 0, open, nan, 1, 1, 1); });
    rejects([&] { OrderBirth::fill_evaluation(2, 0, BirthCursor{}, 100, 1, 1, 1); });
    rejects([&] { BirthCursor::point(BirthCursorDomain::HistoricalPath, 4, 4); });
    rejects([&] { BirthCursor::point(BirthCursorDomain::HistoricalPath, 0, 3); });
    rejects([&] { BirthCursor::segment(BirthCursorDomain::HistoricalPath, 3, 4); });
    rejects([&] { BirthCursor::point(BirthCursorDomain::None, 0, 4); });
}
}

int main() {
    value_contract();
    DirectCascade direct;
    direct.run(bars, 2);
    CHECK(direct.observed.size() == 3);
    if (direct.observed.size() >= 3) {
        const auto& first = direct.observed[0];
        CHECK(first.from_fill() && first.first_fill() == 1 && first.last_fill() == 1);
        CHECK(first.cursor().domain() == BirthCursorDomain::HistoricalPath);
        CHECK(first.cursor().first_point() && first.cursor().count() == 4);
        CHECK(first.bar() == 1 && first.timestamp() == 60000 && first.cursor_price() == 100);
        CHECK(direct.after_direct_fill.first_fill() == 1);
        CHECK(direct.observed[1].first_fill() == 2 && direct.observed[1].last_fill() == 2);
        CHECK(direct.observed[1].evaluation_ordinal() == 2);
        CHECK(direct.observed[2].cause() == OrderBirthCause::ChartEvaluation);
        CHECK(direct.observed[2].first_fill() == 0);
        CHECK(direct.cloned_command.cause() == OrderBirthCause::DirectCommand);
        CHECK(direct.replaced_birth.cause() == OrderBirthCause::ChartEvaluation);
        CHECK(direct.replaced_birth.bar() == 0);
        CHECK(direct.replacement_birth.from_fill() && direct.replacement_birth.first_fill() == 1);
        CHECK(direct.replacement_birth.bar() == 1);
        CHECK(direct.replacement_incarnation > direct.replaced_incarnation);
        CHECK(direct.replacement_priority == direct.replaced_priority);
    }
    const uint64_t original_hash = direct.broker_state_hash();
    DirectCascade copied(direct);
    CHECK(copied.broker_state_hash() == original_hash);
    copied.direct("external-command");
    CHECK(copied.get("external-command").birth.cause() == OrderBirthCause::DirectCommand);
    CHECK(direct.broker_state_hash() == original_hash);
    const Bar extended[] = {bars[0], bars[1], {108, 109, 107, 108, 1, 120000}};
    DirectCascade prefix, complete;
    prefix.set_broker_state_hash_recording(true);
    complete.set_broker_state_hash_recording(true);
    prefix.run(extended, 2);
    complete.run(extended, 3);
    CHECK(prefix.recorded_hashes().size() == 2 && complete.recorded_hashes().size() == 3);
    if (prefix.recorded_hashes().size() == 2 && complete.recorded_hashes().size() == 3) {
        CHECK(prefix.recorded_hashes()[0] == complete.recorded_hashes()[0]);
        CHECK(prefix.recorded_hashes()[1] == complete.recorded_hashes()[1]);
    }
    CHECK(prefix.get("witness-0").birth.first_fill() == complete.get("witness-0").birth.first_fill());
    CHECK(prefix.get("witness-0").birth.cursor().index() == complete.get("witness-0").birth.cursor().index());
    prefix.run(nullptr, 0);
    CHECK(prefix.pending_count() == 0 && prefix.recorded_hashes().empty());
    for (int field = 0; field < 9; ++field) {
        DirectCascade changed(direct);
        const auto receipt = OrderBirth::fill_evaluation(
            field == 0 ? 2 : 1, field == 1 ? 60001 : 60000,
            field == 2 ? BirthCursor::segment(BirthCursorDomain::HistoricalPath, 0, 4)
                       : BirthCursor::point(field == 3 ? BirthCursorDomain::MagnifierTicks : BirthCursorDomain::HistoricalPath,
                           field == 4 ? 1 : 0, field == 3 ? 8 : 4),
            field == 5 ? 101 : 100, field == 6 ? 2 : 1,
            field == 7 || field == 6 ? 2 : 1, field == 8 ? 2 : 1);
        changed.set_birth("witness-0", receipt);
        CHECK(changed.broker_state_hash() != original_hash);
    }
    LaterOpenPolicy policy;
    policy.run(bars, 2);
    CHECK(policy.trailing_birth.from_fill() && policy.priced_birth.from_fill());
    CHECK(policy.trailing_birth.first_fill() == 2 && policy.priced_birth.first_fill() == 2);
    CHECK(policy.trailing_birth.cursor().first_point());
    CHECK(policy.trailing_birth.evaluation_ordinal() == 2);
    CHECK(!compat::pine::historical_cascade_reach(policy.get("trailing")));
    CHECK(compat::pine::historical_cascade_reach(policy.get("priced")));
    policy.clear_trailing_trigger("trailing");
    CHECK(!compat::pine::historical_cascade_reach(policy.get("trailing")));
    CHECK(policy.get("trailing").birth.first_fill() == 2);
    pf_pending_order_v1_t mirror{};
    fill_pending_order_mirror(policy.get("trailing"), &mirror);
    CHECK(mirror.created_during_coof_recalc == 1 && mirror.coof_born_mid_bar == 0);
    CHECK(mirror.birth_first_fill == 2 && mirror.birth_cursor_index == 0);
    CHECK(mirror.birth_evaluation_ordinal == 2);
    SegmentOrigin segment;
    segment.run(bars, 2);
    CHECK(segment.receipt.from_fill());
    CHECK(segment.receipt.cursor().position() == BirthCursorPosition::Segment);
    CHECK(segment.receipt.cursor().index() == 1 && segment.receipt.cursor_price() == 105);
    CHECK(segment.receipt.evaluation_ordinal() == 1);
    CHECK(!compat::pine::first_open_fill_evaluation(segment.receipt));
    ProducerOrigins producers;
    producers.run(bars, 2);
    CHECK(producers.births.size() == 3);
    for (const auto& birth : producers.births) CHECK(birth.from_fill() && birth.first_fill() == 1);
    ProducerOrigins magnified;
    magnified.run(bars, 2, "1", "1", true, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(magnified.births.size() == 3);
    for (const auto& birth : magnified.births) {
        CHECK(birth.from_fill() && birth.first_fill() == 1);
        CHECK(birth.cursor().domain() == BirthCursorDomain::MagnifierTicks);
        CHECK(birth.cursor().first_point() && birth.cursor().count() == 4);
    }
    ThrowsInFill throwing;
    try { throwing.run(bars, 2); } catch (const std::runtime_error&) {}
    throwing.direct("after-throw");
    CHECK(throwing.get("after-throw").birth.cause() == OrderBirthCause::DirectCommand);
    std::printf("order birth provenance: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
