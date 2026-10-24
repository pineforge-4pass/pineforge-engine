// U01 RED/GREEN fixture. On the frozen header closure, only the generated
// std::vector idiom is available; its test reaches the bound and FAILS there.
// On the new closure the same native host callback uses ScriptArray instead.
#pragma once

#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/native_host.hpp>

#if __has_include(<pineforge/array.hpp>)
#include <pineforge/array.hpp>
#include "array_store_fixture.hpp"
#define PF_TEST_HAS_SCRIPT_ARRAY 1
#else
#define PF_TEST_HAS_SCRIPT_ARRAY 0
#endif

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace array_growth_fixture {

inline void require(bool condition, const char* text) {
    if (!condition) throw std::runtime_error(text);
}

// This ceiling is independent of the test population. Store preparation
// provisions eight 4 KiB pages. Array operations keep a 32-byte header and
// scalar cells on the stack; even a copying codec uses at most one 4 KiB
// transfer buffer per nesting level. For this scalar row, charging all eight
// store pages plus one transfer page to POST-preparation retained heap is
// conservative. Fixed Store/host setup and caller-owned OHLCV are excluded;
// neither a measured resident value nor vector capacity sets the expectation.
inline constexpr std::size_t kPageBytes = 4096;
inline constexpr std::size_t kResidentPages = 8;
inline constexpr std::size_t kResidentBound =
    (kResidentPages + 1) * kPageBytes;

inline double close_at(std::size_t bar) {
    // Unique exact binary fractions across both populations. A circular
    // buffer cannot pass late reads just because the feed repeats a period.
    return 100.0 + static_cast<double>(bar) * 2.0 +
           static_cast<double>((bar * 37) % 8) * 0.25;
}

inline double element_at(const std::vector<double>& values, std::size_t index) {
    return values.at(index);
}

#if PF_TEST_HAS_SCRIPT_ARRAY
inline double element_at(const pineforge::ScriptArray<double>& values, std::size_t index) {
    return values.get_raw(static_cast<std::int64_t>(index));
}
#endif

// Allocator bookkeeping is shared with the neighbouring retained-heap rows.
// Only array calls enable recording: caller-owned OHLCV and unrelated host
// allocations are outside this family's measurement. Deallocation of a
// previously recorded block occurs inside the same operation window.
struct OperationWindow {
    bool previous = global_allocation::recording;
    OperationWindow() { global_allocation::recording = true; }
    ~OperationWindow() { global_allocation::recording = previous; }
};

template<class Container>
class AppendHost final : public pineforge::NativeStrategyHost {
    Container& values_;
    std::size_t callbacks_ = 0;
    std::size_t high_water_ = 0;

public:
    explicit AppendHost(Container& values) : values_(values) {
        pineforge::NativeRunSpec spec;
        spec.identity = {"u01-growth", 1};
        spec.input_tf = "1";
        spec.script_tf = "1";
        spec.ticker = "ARRAY";
        spec.tickerid = "TEST:ARRAY";
        spec.type = "crypto";
        spec.currency = "USDT";
        spec.basecurrency = "ETH";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.slot_label_policy = pineforge::NativeSlotLabelPolicy::FeedTolerant;
        spec.initial_capital = 1000000.0;
        spec.point_value = 1.0;
        spec.account_fx = 1.0;
        spec.price_tick = 0.25;
        spec.event_retention = pineforge::NativeEventRetention::Window;
        require(configure_native(spec).status == pineforge::NativeSetupStatus::Applied,
                "configure array growth host");
    }

    void on_native_bar(const pineforge::Bar& bar,
                       const pineforge::NativeDecisionContext&) override {
        OperationWindow window;
        require(bar.close == close_at(callbacks_), "unexpected synthetic script bar");
        values_.push_back(bar.close); // Exactly the generated container idiom.
        ++callbacks_;
        if (global_allocation::live_bytes > high_water_)
            high_water_ = global_allocation::live_bytes;
    }

    std::size_t callbacks() const { return callbacks_; }
    std::size_t high_water() const { return high_water_; }
};

template<class Container>
inline std::size_t drive(Container& values, std::size_t count) {
    std::vector<pineforge::Bar> bars(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double close = close_at(i);
        bars[i] = {close, close + 1.0, close - 1.0, close, 1000.0,
                   INT64_C(1736121600000) + static_cast<std::int64_t>(i) * 60000};
    }
    AppendHost<Container> host(values);
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(host.last_error().empty(), host.last_error().c_str());
    require(host.callbacks() == count, "fixture did not reach its claimed bar count");
    require(values.size() == count, "an appended element was refused or dropped");
    auto high_water = host.high_water();
    {
        OperationWindow window;
        // Read the WHOLE old population only after the last bar. The first
        // element is far outside the resident window in both large rows.
        for (std::size_t i = 0; i < count; ++i) {
            require(element_at(values, i) == close_at(i), "late array read changed");
            if (global_allocation::live_bytes > high_water)
                high_water = global_allocation::live_bytes;
        }
    }
    return high_water;
}

inline int run(std::size_t count, int argc, char** argv) {
    try {
        require(count * sizeof(double) > kResidentBound, "fixture cannot cross its bound");
        const bool vector_mode =
            !PF_TEST_HAS_SCRIPT_ARRAY ||
            (argc == 2 && std::strcmp(argv[1], "--vector") == 0);
        require(argc == 1 || (argc == 2 && std::strcmp(argv[1], "--vector") == 0),
                "usage: growth-test [--vector]");
        std::size_t resident = 0;
        if (vector_mode) {
            std::vector<double> values;
            resident = drive(values, count);
            // Destroy marked blocks while the counter is enabled too.
            {
                OperationWindow window;
                std::vector<double> empty;
                empty.swap(values);
            }
        } else {
#if PF_TEST_HAS_SCRIPT_ARRAY
            array_test::StoreFixture fixture(static_cast<std::uint32_t>(kResidentPages));
            pineforge::ScriptArray<double> values(*fixture.store);
            const auto identity = values.id();
            resident = drive(values, count);
            require(values.id() == identity, "array identity changed on growth");
            const auto stats = fixture.store->stats();
            require(stats.bytes_backing > count * sizeof(double),
                    "payload did not exceed the resident store window");
#endif
        }
        std::printf("U01 bars=%zu retained_operation_heap=%zu bound=%zu mode=%s\n",
                    count, resident, kResidentBound, vector_mode ? "vector-RED" : "store-GREEN");
        require(resident <= kResidentBound, "array resident growth exceeds the derived fixed bound");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "U01 growth: %s\n", error.what());
        return 1;
    }
}

} // namespace array_growth_fixture
