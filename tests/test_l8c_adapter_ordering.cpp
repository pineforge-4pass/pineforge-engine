// A39 P0-11/P1-13/P1-14: every source-policy ordering decision must be a
// strict weak order with explicit deterministic tie and cohort keys.
#include <pineforge/source/pine_native_host.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <variant>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr) do {                                                        \
    ++checks;                                                                   \
    if (!(expr)) {                                                              \
        ++failures;                                                             \
        std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #expr);           \
    }                                                                           \
} while (false)

template<class Tag, typename Tag::type Member>
struct PrivateAccess {
    friend typename Tag::type access(Tag) { return Member; }
};

template<class Tag, auto Member>
struct AutoPrivateAccess {
    friend auto access(Tag) { return Member; }
};

struct PlacementTag {
    using type = source::PlacementTable source::PineExecutionAdapter::*;
    friend type access(PlacementTag);
};
template struct PrivateAccess<PlacementTag,
    &source::PineExecutionAdapter::placement_>;

struct LiveHandlesTag {
    using type = std::vector<no::RequestHandle> source::PineExecutionAdapter::*;
    friend type access(LiveHandlesTag);
};
template struct PrivateAccess<LiveHandlesTag,
    &source::PineExecutionAdapter::live_handles_>;

struct CohortsTag {
    friend auto access(CohortsTag);
};
template struct AutoPrivateAccess<CohortsTag,
    &source::PineExecutionAdapter::cohorts_by_id_>;

Bar bar(std::int64_t timestamp, double price = 100.0) {
    return {price, price, price, price, 1.0, timestamp};
}

std::vector<std::string> accepted_labels(const source::PineNativeHost& host) {
    std::vector<std::string> labels;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        if (const auto* accepted = std::get_if<no::AcceptedEvent>(&*row.command))
            labels.push_back(accepted->request().label);
    }
    return labels;
}

std::vector<std::string> cancelled_labels(const source::PineNativeHost& host) {
    std::vector<std::string> labels;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        if (const auto* cancelled = std::get_if<no::CancelledEvent>(&*row.command))
            labels.push_back(cancelled->request().label);
    }
    return labels;
}

class StopQueueProbe final : public source::PineNativeHost {
public:
    StopQueueProbe() {
        source::PineStrategyConfig config;
        config.initial_capital = 1'000'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 3;
        config.calc_on_order_fills = true;
        configure_pine_strategy(config);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        strategy_entry("A", true,  std::numeric_limits<double>::quiet_NaN(), 5.0);
        strategy_entry("C", true,  std::numeric_limits<double>::quiet_NaN(), 3.0);
        strategy_entry("B", false, std::numeric_limits<double>::quiet_NaN(), 5.0);
    }
};

void mixed_side_stop_order_is_legacy_deterministic() {
    StopQueueProbe host;
    const Bar bars[] = {bar(1'000)};
    host.fixture_retain_all_events();
    host.run(bars, 1, "1", "1");
    CHECK(host.last_error().empty());
    CHECK(accepted_labels(host) == std::vector<std::string>({"A", "C", "B"}));
}

class CohortCancellationProbe final : public source::PineNativeHost {
public:
    CohortCancellationProbe(std::string first, std::string second)
        : first_(std::move(first)), second_(std::move(second)) {
        source::PineStrategyConfig config;
        config.initial_capital = 1'000'000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 2;
        configure_pine_strategy(config);
    }
    std::vector<std::string> cohort_iteration;
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0) strategy_entry(first_, true);
        if (pine_bar_index() == 1) {
            strategy_exit("X-" + first_, first_,
                          std::numeric_limits<double>::quiet_NaN(), 50.0);
            strategy_entry(second_, true);
        }
        if (pine_bar_index() == 2) {
            strategy_exit("X-" + second_, second_,
                          std::numeric_limits<double>::quiet_NaN(), 50.0);
            strategy_order("REV", false, 3.0);
            const auto& cohorts = adapter_.*access(CohortsTag{});
            for (const auto& row : cohorts) {
                cohort_iteration.push_back(row.first);
            }
        }
    }
private:
    std::string first_;
    std::string second_;
};

void reversal_cancels_cohorts_by_sorted_source_id() {
    const Bar bars[] = {bar(1'000), bar(2'000), bar(3'000), bar(4'000), bar(5'000)};
    bool found_unsorted = false;
    for (int first = 0; first < 30 && !found_unsorted; ++first) {
        for (int second = first + 1; second < 30; ++second) {
            const std::string left = "cohort-" + std::to_string(first);
            const std::string right = "cohort-" + std::to_string(second);
            const std::vector<std::string> sorted{left, right};
            CohortCancellationProbe host(left, right);
            host.fixture_retain_all_events();
    host.run(bars, 5, "1", "1");
            CHECK(host.last_error().empty());
            if (host.cohort_iteration != sorted) {
                found_unsorted = true;
                std::vector<std::string> exits;
                for (const auto& label : cancelled_labels(host)) {
                    if (label.rfind("X-", 0) == 0) exits.push_back(label);
                }
                const std::vector<std::string> expected{
                    "X-" + left, "X-" + right};
                CHECK(exits == expected);
                break;
            }
        }
    }
    CHECK(found_unsorted);
}

class EqualCommandTieProbe final : public source::PineNativeHost {
public:
    EqualCommandTieProbe() {
        source::PineStrategyConfig config;
        config.initial_capital = 150.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 1;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        config.slippage = 0;
        config.commission_value = 0.0;
        configure_pine_strategy(config);
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        strategy_entry("A", true);
        strategy_entry("B", false);
        adapter_.flush_pending_entries();

        auto& placement = adapter_.*access(PlacementTag{});
        auto& live = adapter_.*access(LiveHandlesTag{});
        CHECK(live.size() == 2);
        if (live.size() != 2) return;
        const auto tied = placement.at(live.front().incarnation).command_sequence;
        placement.at(live.back().incarnation).command_sequence = tied;
        for (const auto& handle : live) {
            auto& snapshot = placement.at(handle.incarnation);
            if (snapshot.source_id == "B") snapshot.projection_over_pyramiding = true;
        }
        std::reverse(live.begin(), live.end());
    }
};

void equal_command_keys_use_source_sequence_as_the_stable_tie() {
    EqualCommandTieProbe host;
    const Bar bars[] = {bar(1'000), bar(2'000), bar(3'000)};
    host.fixture_retain_all_events();
    host.run(bars, 3, "1", "1");
    CHECK(host.last_error().empty());
    const auto cancelled = cancelled_labels(host);
    CHECK(std::find(cancelled.begin(), cancelled.end(), "B") == cancelled.end());
    CHECK(std::find(cancelled.begin(), cancelled.end(), "A") == cancelled.end());
}

} // namespace

int main(int argc, char** argv) {
    const std::string selected = argc > 1 ? argv[1] : "all";
    if (selected == "all" || selected == "p0-11")
        mixed_side_stop_order_is_legacy_deterministic();
    if (selected == "all" || selected == "p1-13")
        reversal_cancels_cohorts_by_sorted_source_id();
    if (selected == "all" || selected == "p1-14")
        equal_command_keys_use_source_sequence_as_the_stable_tie();
    std::printf("L8c adapter ordering: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
