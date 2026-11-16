// Count real closed-row folds during a burst of deferred applied
// notifications and the normal run-end verification. No clock measurements.
#include <pineforge/native_host.hpp>

#include "../src/native_execution_consumer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <variant>

#ifdef NDEBUG
#error "test_closed_rows_debug_fold_slope requires a Debug engine"
#endif

namespace {

using namespace pineforge;
namespace no = pineforge::native_order;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class BurstHost final : public NativeStrategyHost {
public:
    explicit BurstHost(std::size_t rows) : rows_(rows) {}

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        require(!calculated_, "unexpected second calculation");
        calculated_ = true;
        // Each pair books one row synchronously. All 2*rows notifications
        // wait until this callback returns, so early notifications have a
        // long pending suffix that must not be folded just for Debug.
        for (std::size_t index = 0; index < rows_; ++index) {
            apply(no::Request{no::Transact{1.0}, "open", ""}, 0);
            apply(no::Request{no::Flatten{}, "close", ""}, 1);
        }
        require(notifications_ == 0, "applied callbacks were not deferred");
        require(trades_.size() == rows_, "burst did not book every row");
    }

    void on_native_applied(const no::ExecutionAppliedEvent& event,
                           const NativeDecisionContext&) override {
        require(event.first_trade_index == notifications_ / 2,
                "unexpected applied row index");
        require(event.closed_trade_count == notifications_ % 2,
                "unexpected applied row count");
        ++notifications_;
    }

    std::size_t notifications() const noexcept { return notifications_; }
    std::size_t closed_rows() const { return trades_.size(); }

private:
    void apply(const no::Request& request, std::size_t closed_rows) {
        const auto submitted = submit(request);
        require(submitted.status == no::SubmitStatus::Accepted && submitted.handle,
                "current request was not accepted");
        NativeCurrentExecution command;
        command.target = *submitted.handle;
        const auto result = execute_current(command);
        const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&result);
        require(applied && applied->closed_trade_count == closed_rows,
                "current execution did not book the expected rows");
    }

    const std::size_t rows_;
    std::size_t notifications_ = 0;
    bool calculated_ = false;
};

std::uint64_t measure(std::size_t rows) {
    NativeRunSpec spec;
    spec.identity = {"closed-rows-debug-fold-slope", 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "TEST:CLOSED-ROWS";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10'000;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0;
    spec.close_execution = NativeCloseExecution::AfterCalculation;
    spec.event_retention = NativeEventRetention::Full;

    BurstHost host(rows);
    require(host.configure_native(spec).status == NativeSetupStatus::Applied,
            "native setup failed: " + host.last_error());
    const Bar bar{100, 100, 100, 100, 1, 1'735'689'600'000LL};
    NativeExecutionConsumer::reset_debug_closed_row_fold_work();
    host.run(&bar, 1);
    // A completed run has passed the real verify_closed_rows call; counting
    // the callbacks also establishes that invoke_applied_callback ran.
    require(host.last_error().empty(), "native run failed: " + host.last_error());
    require(host.native_state().kind == NativeLifecycleKind::Completed,
            "native run did not complete");
    require(host.notifications() == 2 * rows, "not every applied callback ran");
    require(host.closed_rows() == rows, "wrong final closed-row population");
    const auto work = NativeExecutionConsumer::debug_closed_row_fold_work();
    std::printf("closed_rows=%zu fold_work=%llu\n",
                rows, static_cast<unsigned long long>(work));
    return work;
}

}  // namespace

int main() {
    try {
        // The base does rows*(rows+1) callback folds plus rows verification
        // folds. The fix needs rows prefix folds plus rows verification folds.
        constexpr std::array<std::size_t, 3> sizes{{64, 128, 256}};
        std::array<std::uint64_t, 3> work{};
        for (std::size_t index = 0; index < sizes.size(); ++index)
            work[index] = measure(sizes[index]);
        for (std::size_t index = 0; index < sizes.size(); ++index)
            require(work[index] >= 2 * sizes[index],
                    "counter missed prefix or verification folds");
        require(work[1] <= 2 * work[0] + 8,
                "closed-row fold work exceeds linear growth at 2N");
        require(work[2] <= 4 * work[0] + 16,
                "closed-row fold work exceeds linear growth at 4N");
        require(work[2] <= 2 * work[1] + 8,
                "closed-row fold work exceeds linear growth from 2N to 4N");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_closed_rows_debug_fold_slope: %s\n", error.what());
        return 1;
    }
    return 0;
}
