#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cassert>
#include <cstdint>

namespace {

using namespace pineforge;
using source::PineRequestReadAction;

constexpr int sub_bars = 4; // one "240" chart bar holds four "60" slots

struct Probe final : source::PineStrategyHost {
    explicit Probe(int first_check_) : first_check(first_check_) {}

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_lower_tf_eval(0, "60", input_tf_);
    }

    void on_source_bar(const Bar&) override {}

    void evaluate_security(int sec_id, const Bar&, bool publish) override {
        assert(sec_id == 0 && publish);
        const auto delivery = current_request_delivery(sec_id);
        assert(delivery.caller_valid && !delivery.eligible);
        assert(delivery.requested_slot == deliveries);
        const auto check_child = [&](std::uint64_t parent, std::uint64_t caller) {
            auto child = make_runtime_security_cursor("240", true, false, false);
            assert(!current_request_read(child).caller_valid);
            int child_deliveries = 0;
            child.feed({first, 0, 1}, [&](const Bar& value, bool complete) {
                ++child_deliveries;
                assert(complete && value.close == first.close);
                const auto facts = current_request_delivery(child);
                assert(facts.caller_valid && facts.caller == caller);
                assert(facts.actual_complete && !facts.eligible);
            }, [] {});
            assert(child_deliveries == 1);
            assert(child.parent_caller == parent);
            const auto binding = current_request_read(child);
            assert(binding.caller_valid && binding.caller == caller);
            assert(binding.action == PineRequestReadAction::BaseCurrent);
            ++checks;
        };
        // A child reads the body caller of the slot it runs in and takes two
        // callers of its own. In the first chart bar the slot at first_check
        // follows that many slots that had no child, so its body caller is
        // the next one after theirs; the slot after it follows the child.
        const std::uint64_t body =
            delivery.caller + 1 + static_cast<std::uint64_t>(first_check);
        if (deliveries == first_check) check_child(body, body + 1);
        if (deliveries == first_check + 1 && deliveries < sub_bars)
            check_child(body + 3, body + 4);
        ++deliveries;
    }

    const int first_check;
    Bar first{};
    int deliveries = 0;
    int checks = 0;
};

// The control lives in this namespace so that its names are found before any
// name of the engine's namespaces.
void nested_security_caller_control() {
    constexpr std::int64_t origin = 1'704'067'200'000;
    constexpr std::int64_t chart_ms = 240 * 60'000LL;
    const std::array<Bar, 2> bars{{
        {100, 102, 99, 101, 240, origin},
        {101, 103, 100, 102, 240, origin + chart_ms},
    }};
    // One run per slot of the first chart bar: every slot is read once after
    // slots without a child, and every slot but the first once after a child.
    for (int first_check = 0; first_check < sub_bars; ++first_check) {
        Probe strategy(first_check);
        strategy.set_syminfo_timezone("UTC");
        strategy.set_syminfo_session("24x7");
        strategy.set_syminfo_type("crypto");
        strategy.first = bars[0];
        strategy.run(bars.data(), 2, "240", "240");
        assert(strategy.last_error().empty());
        assert(strategy.deliveries == 2 * sub_bars);
        assert(strategy.checks == (first_check + 1 < sub_bars ? 2 : 1));
    }
}

} // namespace

int main() {
    nested_security_caller_control();
}
