// CR-150 L19D: exercise the real adapter before testing its storage helper.
// This first witness uses only the base's API: it must compile on d61e68b9e
// and fail because completed same-id openings remain in OriginRoster, not
// because a new declaration, sink option or module is missing. Execution of
// that red tree and the final Release/Debug proof belong to CORE-TOP's lab.
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;

int checks = 0;
int failures = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                 \
    if (!(condition)) {                                                       \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                           \
    }                                                                         \
} while (false)

// The existing ordering witnesses use this explicit-instantiation access
// pattern as well. No production fixture switch or public API is added.
template<class Tag, auto Member>
struct PrivateAccess {
    friend auto access(Tag) { return Member; }
};
struct CohortsTag { friend auto access(CohortsTag); };
template struct PrivateAccess<CohortsTag, &source::PineExecutionAdapter::cohorts_by_id_>;

std::vector<Bar> tape(int count) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1.0, kStart + i * 60000LL});
    return bars;
}

class CycleHost final : public source::PineStrategyHost {
public:
    CycleHost(int cycles, bool cancelled, bool recording)
        : cycles_(cycles), cancelled_(cancelled) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.process_orders_on_close = true;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        set_broker_state_hash_recording(recording);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i < cycles_ * 6) {
            if (i % 6 == 0)
                strategy_entry("entry", true, cancelled_ ? 50.0 : kNa, kNa, 1.0);
            if (i % 6 == 2) {
                if (cancelled_) strategy_cancel("entry");
                else strategy_close("entry");
            }
        }
        if (const auto* roster = origins()) {
            peak_members_ = std::max(peak_members_, roster->members().size());
        }
    }

    const source::OriginRoster* origins() const {
        const auto& cohorts = adapter_.*access(CohortsTag{});
        const auto found = cohorts.find("entry");
        return found == cohorts.end() ? nullptr : &found->second.origins;
    }
    std::size_t peak_members() const { return peak_members_; }

    void hash_host_extension(BrokerStateHashSink& sink) const override {
        source::PineStrategyHost::hash_host_extension(sink);
        sink.s("cr150-l19d/cycles");
        sink.i(cycles_); sink.i(cancelled_ ? 1 : 0); sink.u(peak_members_);
    }

private:
    int cycles_;
    bool cancelled_;
    std::size_t peak_members_ = 0;
};

void completed_cycles_drop_the_retained_prefix(int cycles, bool cancelled, bool recording) {
    CycleHost host(cycles, cancelled, recording);
    const auto bars = tape(cycles * 6 + 8);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    CHECK(host.last_error().empty());
    const auto* roster = host.origins();
    CHECK(roster != nullptr);
    if (!roster) return;
    // This is the base's first expected runtime failure: the historical
    // vector still has `cycles` members after all lots/requests ended.
    CHECK(roster->members().empty());
    CHECK(host.peak_members() <= 2);
    // Retirement must preserve the total accepted count used by hash_state.
    CHECK(roster->size() == static_cast<std::size_t>(cycles));
    CHECK(host.trade_count() == (cancelled ? 0 : cycles));
    if (!cancelled) {
        for (int i = 0; i < host.trade_count(); ++i) {
            const auto& trade = host.get_trade(i);
            CHECK(trade.entry_id == "entry");
            CHECK(trade.qty == 1.0);
            CHECK(trade.entry_price == 100.0);
            CHECK(trade.exit_price == 100.0);
        }
    }
}

class HeldOriginHost final : public source::PineStrategyHost {
public:
    static constexpr int kCancelled = 24;
    static constexpr int kCloseBar = kCancelled * 4 + 8;

    HeldOriginHost() {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.process_orders_on_close = true;
        config.pyramiding = 2;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == 0) strategy_entry("entry", true, kNa, kNa, 1.0);
        if (i >= 4 && i < kCloseBar - 4) {
            if (i % 4 == 0) strategy_entry("entry", true, 50.0, kNa, 1.0);
            if (i % 4 == 2) strategy_cancel("entry");
        }
        if (i == kCloseBar) {
            const auto* roster = origins();
            CHECK(roster != nullptr);
            if (roster) {
                // Later cancellations cannot move a prefix past the first
                // origin while its lot remains in the kernel cohort.
                CHECK(roster->members().size() == roster->size());
                CHECK(roster->size() == static_cast<std::size_t>(kCancelled + 1));
            }
            CHECK(live_position_size() == 1.0);
            strategy_close_all();
        }
    }

    const source::OriginRoster* origins() const {
        const auto& cohorts = adapter_.*access(CohortsTag{});
        const auto found = cohorts.find("entry");
        return found == cohorts.end() ? nullptr : &found->second.origins;
    }
};

void an_open_oldest_lot_pins_the_prefix_until_release() {
    HeldOriginHost host;
    const auto bars = tape(HeldOriginHost::kCloseBar + 9);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    const auto* roster = host.origins();
    CHECK(roster != nullptr);
    if (!roster) return;
    CHECK(roster->members().empty());
    CHECK(roster->size() == static_cast<std::size_t>(HeldOriginHost::kCancelled + 1));
}

} // namespace

int main() {
    completed_cycles_drop_the_retained_prefix(10, false, false);
    completed_cycles_drop_the_retained_prefix(256, false, false);
    completed_cycles_drop_the_retained_prefix(96, false, true);
    completed_cycles_drop_the_retained_prefix(96, true, false);
    an_open_oldest_lot_pins_the_prefix_until_release();
    std::printf("OriginRoster retirement: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
