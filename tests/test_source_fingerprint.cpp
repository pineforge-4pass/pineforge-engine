// Source-adapter v2 fingerprint coverage. L3a deliberately stops hashing the
// retired PendingOrder/mixin owner and folds the durable adapter+scheduler
// state that drives a switched PineStrategyHost instead.
#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include "../src/broker_state_hash_internal.hpp"

#include <cstdio>

using namespace pineforge;

namespace {

int failures = 0;

#define CHECK(condition) do {                                                     \
    if (!(condition)) {                                                           \
        std::fprintf(stderr, "FAIL %s:%d  %s\\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                               \
    }                                                                             \
} while (0)

class SourceProbe final : public source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {}

    // N5: the adapter folds through the kernel's generic host seam; the
    // deprecated spelling is a forward to it.
    std::uint64_t generic_extension_hash() const {
        BrokerStateHashSink sink;
        hash_host_extension(sink);
        return sink.h;
    }
    std::uint64_t deprecated_extension_hash() const {
        BrokerStateHashSink sink;
        hash_source_extension(sink);
        return sink.h;
    }

    void mutate_configuration() {
        source::PineStrategyConfig config;
        config.initial_capital = 12345.0;
        config.default_qty_type = static_cast<int>(QtyType::CASH);
        config.default_qty_value = 77.0;
        config.pyramiding = 3;
        configure_pine_strategy(config);
    }

    void mutate_staged_ingress() {
        source::StagedConfiguration staged;
        staged.syminfo.tickerid = "HASH:STAGED";
        staged.syminfo.timezone = "Asia/Taipei";
        staged.inputs.emplace("period", "17");
        staged.account_fx = 1.25;
        staged.account_fx_effective_from_ms.push_back(1000);
        staged.account_fx_per_quote.push_back(1.5);
        staged.quantity_grid = 0.25;
        adapter_.set_staged_configuration(staged);
    }

    void mutate_risk() {
        adapter_.set_risk_direction(-1);
        adapter_.set_risk_max_cons_loss_days(2);
        adapter_.set_risk_max_drawdown(12.5, true);
        adapter_.set_risk_max_intraday_loss(8.5, false);
        adapter_.set_risk_max_position_size(7.0);
    }

    void mutate_cap_and_priority() {
        adapter_.attach_execution_adapter();
        adapter_.cap = 2;
    }

    void mutate_scheduler() {
        const Bar bars[] = {{100.0, 101.0, 99.0, 100.5, 1.0, 60000}};
        const NativeBeginArgs args{bars, 1, "1", "1", false, 4,
            MagnifierDistribution::ENDPOINTS, false, 2, 64,
            nullptr, nullptr, nullptr, false, 0};
        scheduler_.capture_begin(args);
    }
};

class NativeProbe final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}

    std::uint64_t source_extension_hash() const {
        BrokerStateHashSink sink;
        hash_source_extension(sink);
        return sink.h;
    }
    std::uint64_t host_extension_hash() const {
        BrokerStateHashSink sink;
        hash_host_extension(sink);
        return sink.h;
    }
};

struct Group {
    const char* name;
    void (SourceProbe::*mutate)();
};

void check_group(const Group& group) {
    SourceProbe first;
    const std::uint64_t before = first.broker_state_hash();
    (first.*group.mutate)();
    const std::uint64_t changed = first.broker_state_hash();
    if (changed == before) {
        std::fprintf(stderr, "FAIL source-adapter group %s: mutation did not change hash\\n",
                     group.name);
        ++failures;
    }

    SourceProbe replay;
    (replay.*group.mutate)();
    if (replay.broker_state_hash() != changed) {
        std::fprintf(stderr, "FAIL source-adapter group %s: replay did not reproduce hash\\n",
                     group.name);
        ++failures;
    }
}

} // namespace

int main() {
    const Group groups[] = {
        {"configuration", &SourceProbe::mutate_configuration},
        {"staged ingress", &SourceProbe::mutate_staged_ingress},
        {"risk", &SourceProbe::mutate_risk},
        {"priority/cap", &SourceProbe::mutate_cap_and_priority},
        {"scheduler", &SourceProbe::mutate_scheduler},
    };
    for (const Group& group : groups) check_group(group);

    NativeProbe native;
    BrokerStateHashSink source_none;
    source_none.s("source:none");
    CHECK(native.source_extension_hash() == source_none.h);
    const std::uint64_t native_before = native.broker_state_hash();
    {
        SourceProbe source;
        source.mutate_configuration();
        source.mutate_staged_ingress();
        source.mutate_risk();
        source.mutate_cap_and_priority();
        source.mutate_scheduler();
    }
    CHECK(native.source_extension_hash() == source_none.h);
    CHECK(native.broker_state_hash() == native_before);

    // N5: one seam, two spellings. A bare host's generic default is the same
    // marker; the adapter's fold is reached through either name, is not the
    // marker, and still follows the adapter's state.
    CHECK(native.host_extension_hash() == source_none.h);
    SourceProbe adapter;
    CHECK(adapter.generic_extension_hash() == adapter.deprecated_extension_hash());
    CHECK(adapter.generic_extension_hash() != source_none.h);
    const std::uint64_t adapter_before = adapter.generic_extension_hash();
    adapter.mutate_risk();
    CHECK(adapter.generic_extension_hash() != adapter_before);
    CHECK(adapter.generic_extension_hash() == adapter.deprecated_extension_hash());
    return failures == 0 ? 0 : 1;
}
