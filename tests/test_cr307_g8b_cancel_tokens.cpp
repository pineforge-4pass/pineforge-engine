// Expected wall time: 3-20 seconds Release on a 10-core host (unmeasured).
// Existing bound characterization, not a red claim: one cancelled id per
// evaluation gives exactly one token, cleared before the next evaluation.
#include "cr307_g8b_fixture.hpp"

namespace {
class CancelHost final : public pineforge::source::PineStrategyHost {
public:
    CancelHost() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1e9;
        config.margin_long = config.margin_short = 0.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }
    void on_source_bar(const pineforge::Bar&) override {
        using namespace cr307_g8b;
        const auto& tokens = adapter_.*access(Tokens{});
        require(tokens.empty(), "begin_source_evaluation cleared previous cancellation");
        const auto id = "cancel-" + std::to_string(pine_bar_index());
        strategy_entry(id, true, 1.0, na, 1.0);
        require(native_working_requests().size() == 1, "fresh limit entry is really live");
        strategy_cancel(id);
        require(native_working_requests().empty(), "fresh entry really cancelled");
        require(tokens.size() == 1 && tokens.find(id) != tokens.end(),
                "only this evaluation's cancelled id retains a token");
        ++callbacks;
    }
    std::size_t callbacks = 0;
};
}

int main() {
    try {
        for (const std::size_t count : {2000U, 8000U}) {
            const auto data = cr307_g8b::chart(count);
            CancelHost host;
            host.run(data.data(), static_cast<int>(data.size()), "1", "1");
            if (!host.last_error().empty()) throw std::runtime_error(host.last_error());
            cr307_g8b::require(host.callbacks == count && host.trade_count() == 0,
                              "every fresh id cancelled without any fill");
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
