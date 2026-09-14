// Source-state fingerprint coverage for the R4-C split. Each case mutates one
// relocated state group, proves the extension changes the broker hash, then
// proves the ordered reset protocol restores the pre-run state.
#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/broker_state_hash_internal.hpp"

#include <cstdint>
#include <cstdio>

using namespace pineforge;

namespace {

int failures = 0;

#define CHECK(condition) do {                                                     \
    if (!(condition)) {                                                           \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                               \
    }                                                                             \
} while (0)

class SourceProbe final : public source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {}

    void reset_for_test() { reset_run_state(); }
    void prepare_none() {}

    void mutate_language_state() { _src_open_.push(101.25); }

    void mutate_pending_intent() {
        source::PendingOrder order{};
        order.id = "source-fingerprint-pending";
        order.type = OrderType::ENTRY;
        order.is_long = true;
        order.qty = 1.0;
        pending_orders_.push_back(order);
    }

    void mutate_admission_journal() {
        const uint64_t sequence = adapter_.admission_journal.next_sequence();
        adapter_.admission_journal.abandon(sequence);
    }

    void prepare_priority_cap() {
        adapter_.priority.attach();
        adapter_.cap = 2;
    }

    void mutate_priority_cap_runtime() {
        compat::pine::CapClock clock{};
        clock.session = "24x7";
        clock.timezone = "UTC";
        clock.chart_day = 7;
        clock.chart_month = 4;
        (void)adapter_.cap.placement(clock);
    }

    void mutate_day_ledger() { cons_loss_day_count_ = 1; }

    void mutate_freeze() {
        pos_view_freeze_bar_ = 17;
        pos_view_frozen_side_ = PositionSide::LONG;
        pos_view_frozen_qty_ = 2.0;
        pos_view_frozen_entry_qty_["L"] = 2.0;
    }
};

class NativeProbe final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}

    uint64_t source_extension_hash() const {
        BrokerStateHashSink sink;
        hash_source_extension(sink);
        return sink.h;
    }
};

struct Group {
    const char* name;
    void (SourceProbe::*prepare)();
    void (SourceProbe::*mutate)();
};

void check_group(const Group& group) {
    SourceProbe first;
    (first.*group.prepare)();
    first.reset_for_test();
    const uint64_t before = first.broker_state_hash();
    (first.*group.mutate)();
    const uint64_t changed = first.broker_state_hash();
    if (changed == before) {
        std::fprintf(stderr, "FAIL source group %s: mutation did not change hash\n", group.name);
        ++failures;
    }

    first.reset_for_test();
    if (first.broker_state_hash() != before) {
        std::fprintf(stderr, "FAIL source group %s: reset did not restore hash\n", group.name);
        ++failures;
    }

    SourceProbe replay;
    (replay.*group.prepare)();
    replay.reset_for_test();
    (replay.*group.mutate)();
    if (replay.broker_state_hash() != changed) {
        std::fprintf(stderr, "FAIL source group %s: replay did not reproduce hash\n", group.name);
        ++failures;
    }
}

} // namespace

int main() {
    SourceProbe detached;
    detached.reset_for_test();
    SourceProbe configured;
    configured.prepare_priority_cap();
    configured.reset_for_test();
    CHECK(detached.broker_state_hash() != configured.broker_state_hash());

    const Group groups[] = {
        {"language state", &SourceProbe::prepare_none, &SourceProbe::mutate_language_state},
        {"pending intent", &SourceProbe::prepare_none, &SourceProbe::mutate_pending_intent},
        {"admission journal", &SourceProbe::prepare_none, &SourceProbe::mutate_admission_journal},
        {"priority/cap", &SourceProbe::prepare_priority_cap,
         &SourceProbe::mutate_priority_cap_runtime},
        {"day ledger", &SourceProbe::prepare_none, &SourceProbe::mutate_day_ledger},
        {"freeze", &SourceProbe::prepare_none, &SourceProbe::mutate_freeze},
    };
    for (const Group& group : groups) check_group(group);

    NativeProbe native;
    BrokerStateHashSink source_none;
    source_none.s("source:none");
    CHECK(native.source_extension_hash() == source_none.h);
    const uint64_t native_before = native.broker_state_hash();

    // Source state constructed in this process cannot enter an already-created
    // native host's default source:none extension.
    {
        SourceProbe source;
        source.prepare_priority_cap();
        source.mutate_language_state();
        source.mutate_pending_intent();
        source.mutate_admission_journal();
        source.mutate_priority_cap_runtime();
        source.mutate_day_ledger();
        source.mutate_freeze();
    }
    CHECK(native.source_extension_hash() == source_none.h);
    CHECK(native.broker_state_hash() == native_before);

    return failures == 0 ? 0 : 1;
}
