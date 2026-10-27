// Public runs pin scanned_rows() and dormant_rows() for unreachable residents;
// both counters follow NativeExecutionConsumer::quiet_points()'s interface.
#include "../src/native_execution_consumer.hpp"

#include <pineforge/native_host.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <stdexcept>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;

void require(bool value, const char* text) {
    if (!value) throw std::runtime_error(text);
}

class Host final : public NativeStrategyHost {
public:
    explicit Host(std::size_t rows) : rows_(rows) {}
    NativeExecutionConsumer& consumer() {
        return as_native_consumer(execution_consumer());
    }
    void on_native_input(const Bar&, const NativeInputContext&) override {
        // Read the real scan counter before matching so later dormant-only
        // bars prove no resident row was evaluated; a constant cannot show that.
        before_ = consumer().scanned_rows();
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++bar_;
        if (bar_ == 1) {
            for (std::size_t i = 0; i < rows_; ++i) {
                const bool buy = (i & 1) == 0;
                no::Request r{no::Transact{buy ? 1.0 : -1.0}, "resident", ""};
                const auto kind = (i / 2) % 4;
                if (kind == 0) r.trigger = no::Limit{buy ? 50.0 : 200.0};
                else if (kind == 1) r.trigger = no::Stop{buy ? 200.0 : 50.0};
                else if (kind == 2)
                    r.trigger = no::StopLimit{buy ? 200.0 : 50.0, buy ? 201.0 : 49.0};
                else
                    // A real stop activation, then an unreachable live
                    // limit leg: exercises OTHER even in StopLimitLive.
                    r.trigger = no::StopLimit{buy ? 90.0 : 110.0, buy ? 50.0 : 200.0};
                require(submit(r).handle.has_value(), "resident order submission refused");
            }
        } else if (bar_ >= 4) {
            // Every row is eligible and unreachable; after the join there
            // are zero active candidates. No per-row evaluation is needed.
            require(consumer().scanned_rows() == before_,
                    "scanned an unreachable dormant resident row");
            require(consumer().dormant_rows() > 0, "did not make any resident row dormant");
            ++checked_bars;
        }
    }
    std::size_t checked_bars = 0;
private:
    std::size_t rows_;
    std::size_t bar_ = 0;
    std::uint64_t before_ = 0;
};

void run(std::size_t rows) {
    Host h(rows);
    NativeRunSpec s;
    s.identity = {"c3-dormant-resident", 1};
    s.input_tf = "5"; s.script_tf = "5";
    s.tickerid = "TEST:C3"; s.timezone = "UTC"; s.session = "24x7";
    s.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    s.initial_capital = 1.0e9; s.point_value = 1; s.account_fx = 1;
    s.report_policy = NativeReportPolicy::HostRecorded;
    require(h.configure_native(s).status == NativeSetupStatus::Applied, "resident book configuration refused");
    std::array<Bar, 8> bars{};
    for (std::size_t i = 0; i < bars.size(); ++i)
        bars[i] = {100, 100, 100, 100, 1,
                   1736121600000LL + static_cast<std::int64_t>(i) * 300000};
    h.run(bars.data(), static_cast<int>(bars.size()));
    require(h.native_state().kind == NativeLifecycleKind::Completed, h.last_error().c_str());
    require(h.checked_bars == bars.size() - 3, "resident observation window did not run");
    require(h.consumer().request_core().live_size() == rows, "lost resting resident rows");
    require(!h.consumer().request_core().book_spilled(), "resident fixture unexpectedly spilled");
    require(h.trade_count() == 0, "unreachable resident book traded");
    require(h.consumer().dormant_rows() > 0 && h.consumer().fast_points() > 0
                && h.consumer().eager_points() == 0 && h.consumer().grid_points() == 0,
            "resident scan check used eager matching or a grid");
}
} // namespace

int main() {
    try {
        run(512);
        run(4096); // The actual kBookMax, not a test override.
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "test_native_dormant_book_resident: %s\n", e.what());
        return 1;
    }
}
