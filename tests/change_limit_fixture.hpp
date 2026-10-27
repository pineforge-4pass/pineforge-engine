#pragma once

#include "a5h_growth_fixture.hpp"
#include <pineforge/run_failure.hpp>
#include <pineforge/ta.hpp>

#include <deque>
#include <optional>
#include <string>

namespace change_limit_test {

using history_growth_test::require;
inline constexpr int kCap = 15000;

inline std::string message(int requested, const std::string& prefix = {}) {
    return prefix +
        "The script attempts to reference historical data that is too far from "
        "the current bar (" + std::to_string(requested) +
        " bars back). The historical buffer's limit is 15000 bars.";
}

inline void check_failure(const std::runtime_error& error, int requested,
                          const std::string& prefix = {}) {
    const auto* typed = dynamic_cast<const pineforge::RunFailureInfo*>(&error);
    require(typed && typed->run_failure_code() == pineforge::RunFailureCode::pine_runtime_limit,
            "CHANGE_CAP_PLUS_ONE_RED: refusal lost its runtime-limit code");
    require(typed->run_failure().args &&
            *typed->run_failure().args == "{\"limit\":\"max_bars_back\",\"max\":15000}",
            "CHANGE_CAP_PLUS_ONE_RED: refusal lost its typed limit arguments");
    require(error.what() == message(requested, prefix),
            "CHANGE_CAP_PLUS_ONE_RED: refusal changed the historical-buffer diagnostic");
#ifdef PINEFORGE_HAS_TA_HISTORY_LIMIT
    const auto* limit = dynamic_cast<const pineforge::ta::HistoryLimitError*>(&error);
    require(limit && limit->requested_offset() == requested && limit->limit() == kCap,
            "CHANGE_CAP_PLUS_ONE_RED: refusal lost the requested offset or limit");
#endif
}

template<class T>
auto hash(const T& value, int) -> decltype(value.state_hash(), std::optional<std::uint64_t>{}) {
    return value.state_hash();
}
template<class T>
std::optional<std::uint64_t> hash(const T&, long) { return std::nullopt; }

inline void boundary() {
    using namespace pineforge;
    ta::Change value(kCap);
    std::deque<double> reference;
    for (int bar = 0; bar <= kCap; ++bar) {
        const double source = history_growth_test::value_at(static_cast<std::size_t>(bar));
        reference.push_front(source);
        double result;
        try { result = value.compute(source, kCap); }
        catch (const std::exception&) {
            throw std::runtime_error("CHANGE_CAP_PASSES_RED: the inclusive cap was refused");
        }
        require(bar < kCap ? is_na(result) : result == reference.front() - reference.back(),
                "CHANGE_CAP_PASSES_RED: cap result differs from the native history");
    }
    const auto before = hash(value, 0);
    {
        ta::BarContextScope context(73, 0);
        for (bool again : {false, true}) {
            bool refused = false;
            try {
                if (again) (void)value.recompute(-901.0, kCap + 1);
                else (void)value.compute(-903.0, kCap + 1);
            } catch (const std::runtime_error& error) {
                refused = true;
                check_failure(error, kCap + 1, "Error on bar 73: ");
            }
            require(refused, "CHANGE_CAP_PLUS_ONE_RED: an over-limit call was accepted");
            require(hash(value, 0) == before,
                    "CHANGE_CAP_PLUS_ONE_RED: a refused call changed the state hash");
        }
    }
    // Inspect every retained slot after both refusals. The expected values
    // come from a native deque of inputs, never another Change instance.
    for (int offset = 0; offset <= kCap; ++offset)
        require(value.recompute(reference.front(), offset) ==
                    reference.front() - reference[static_cast<std::size_t>(offset)],
                "CHANGE_CAP_PLUS_ONE_RED: a refusal changed a retained value");
    require(hash(value, 0) == before,
            "CHANGE_CAP_PLUS_ONE_RED: refusal/readback changed the readable window");
    std::puts("change_limit_boundary: PASS cap=15000 cap_plus_one=15001 state_unchanged");
}

inline int growth(std::size_t count) {
    try {
        using namespace pineforge;
        require(count > static_cast<std::size_t>(kCap + 1),
                "change growth population does not reach the limit");
        require(global_allocation::live_bytes == 0, "change growth measurement starts dirty");
        constexpr std::size_t bound = history_growth_test::kHistoryBound + history_growth_test::kPage;
        // Fixed store workspace + two history pages + one page for the
        // bounded diagnostic and its typed arguments, independent of count.
        std::optional<ta::Change> value;
        history_growth_test::peak = 0;
        global_allocation::on_recorded = history_growth_test::record_peak;
        {
            history_growth_test::Window window;
            value.emplace(1);
        }
        std::size_t refusals = 0;
        auto step = [&](std::size_t bar, double source) {
            history_growth_test::Window window;
            const auto length = static_cast<int>(bar);
            try {
                const double result = value->compute(source, length);
                if (length <= kCap)
                    require(result == source - history_growth_test::value_at(0),
                            "CHANGE_LENGTH_GROWTH_RED: legal growing length changed its value");
            } catch (const std::runtime_error& error) {
                require(length > kCap,
                        "CHANGE_LENGTH_GROWTH_RED: a legal growing length was refused");
                check_failure(error, length);
                ++refusals;
            }
        };
        history_growth_test::drive(count, step);
        const auto resident_peak = history_growth_test::peak;
        const auto expected_refusals = count - static_cast<std::size_t>(kCap + 1);
        const bool red = refusals != expected_refusals || resident_peak > bound;
        if (!red) {
            const double last = history_growth_test::value_at(kCap);
            for (int offset = 0; offset <= kCap; ++offset)
                require(value->recompute(last, offset) ==
                            last - history_growth_test::value_at(static_cast<std::size_t>(kCap - offset)),
                        "CHANGE_LENGTH_GROWTH_RED: refused growth changed the retained history");
        }
        {
            history_growth_test::Window window;
            value.reset();
        }
        global_allocation::on_recorded = nullptr;
        require(global_allocation::live_bytes == 0, "change growth owners leaked heap");
        std::printf("change_length_growth bars=%zu refusals=%zu expected=%zu peak=%zu bound=%zu %s\n",
                    count, refusals, expected_refusals, resident_peak, bound,
                    red ? "CHANGE_LENGTH_GROWTH_RED" : "PASS");
        return red ? 1 : 0;
    } catch (const std::exception& error) {
        global_allocation::on_recorded = nullptr;
        std::fprintf(stderr, "change_length_growth: %s\n", error.what());
        return 2;
    }
}

} // namespace change_limit_test
