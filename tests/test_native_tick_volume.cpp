#include <pineforge/native_host.hpp>

#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v19 {
struct NativeExecutionConsumerProbe {
    static double decimal_sum(double step, int64_t units) {
        NativeExecutionConsumer::TickVolume volume;
        volume.reset(step);
        volume.units = units;
        return volume.value();
    }
    static bool exact(const NativeExecutionConsumer& consumer) {
        return consumer.forming_tick_volume_.exact;
    }
};
}
}

namespace {
using namespace pineforge;
int failures = 0;
int checks = 0;

#define CHECK(expression) do { ++checks; if (!(expression)) { \
    ++failures; std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
} } while (false)

struct Host final : NativeStrategyHost {
    std::vector<Bar> bars;
    double partial_volume = 0.0;
    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        bars.push_back(bar);
    }
    void on_native_tick(const Bar&, const NativeTickContext&) override {
        const auto partial = current_partial_bar();
        CHECK(partial.has_value());
        if (partial) partial_volume = partial->volume;
    }
    NativeExecutionConsumer& consumer() {
        return as_native_consumer(execution_consumer());
    }
};

void begin(Host& host, std::optional<double> step, const std::string& script_tf = "1") {
    NativeRunSpec spec;
    spec.identity = {"tick-volume", 1};
    spec.input_tf = "1";
    spec.script_tf = script_tf;
    spec.tickerid = "TEST:VOLUME";
    spec.timezone = "UTC+0";
    spec.session = "24x7";
    spec.initial_capital = 100000;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.quantity_grid = step;
    CHECK(host.configure_native(spec).status == NativeSetupStatus::Applied);
    const Bar warmup{100, 100, 100, 100, 1, 0};
    CHECK(host.stream_begin(&warmup, 1, "1", script_tf));
    host.bars.clear();
}

void push(Host& host, const std::vector<double>& quantities) {
    std::vector<TradeTick> ticks;
    for (std::size_t index = 0; index < quantities.size(); ++index)
        ticks.push_back({60000 + static_cast<int64_t>(index), index + 1, 100, quantities[index]});
    CHECK(host.stream_push_ticks(ticks.data(), static_cast<int>(ticks.size())));
}

void seal(Host& host) {
    CHECK(host.stream_advance_time(120000));
    CHECK(host.bars.size() == 1);
}

void small_decimal_and_pinned_volume() {
    Host small;
    begin(small, 0.1);
    push(small, {0.1, 0.2});
    CHECK(small.partial_volume == std::strtod("0.3", nullptr));
    CHECK(NativeExecutionConsumerProbe::exact(small.consumer()));
    seal(small);
    CHECK(small.bars.back().volume == std::strtod("0.3", nullptr));

    Host pinned;
    begin(pinned, 0.001);
    std::vector<double> quantities(34231, 0.1);
    quantities.push_back(0.086);
    push(pinned, quantities);
    CHECK(pinned.partial_volume == std::strtod("3423.186", nullptr));
    seal(pinned);
    CHECK(pinned.bars.back().volume == std::strtod("3423.186", nullptr));
}

void compensated_fallbacks() {
    for (const auto step : {std::optional<double>{}, std::optional<double>{0.3}}) {
        Host host;
        begin(host, step);
        push(host, {1e16, 1, 1});
        CHECK(!NativeExecutionConsumerProbe::exact(host.consumer()));
        CHECK(host.partial_volume == 10000000000000002.0);
        seal(host);
        CHECK(host.bars.back().volume == 10000000000000002.0);
    }
    Host off_grid;
    begin(off_grid, 0.1);
    push(off_grid, {0.1, 0.2, 0.05});
    CHECK(!NativeExecutionConsumerProbe::exact(off_grid.consumer()));
    CHECK(off_grid.partial_volume == std::strtod("0.35000000000000003", nullptr));
    seal(off_grid);
    CHECK(off_grid.bars.back().volume == std::strtod("0.35000000000000003", nullptr));
    CHECK(off_grid.stream_push_tick({120001, 4, 100, 0.1}));
    CHECK(NativeExecutionConsumerProbe::exact(off_grid.consumer()));
    CHECK(off_grid.stream_end(true));
    CHECK(off_grid.bars.back().volume == 0.1);

    Host nearby;
    begin(nearby, 0.1);
    push(nearby, {std::nextafter(0.3, 1.0)});
    CHECK(!NativeExecutionConsumerProbe::exact(nearby.consumer()));
    seal(nearby);
    CHECK(nearby.bars.back().volume == std::nextafter(0.3, 1.0));
}

void integer_overflow_and_quiet_slot() {
    Host overflow;
    begin(overflow, 0.001);
    push(overflow, {4e15, 4e15, 4e15, 1, 1});
    CHECK(!NativeExecutionConsumerProbe::exact(overflow.consumer()));
    seal(overflow);
    CHECK(overflow.bars.back().volume == 12000000000000002.0);

    Host single_overflow;
    begin(single_overflow, 1.0);
    push(single_overflow, {std::ldexp(1.0, 63), 1024, 1024});
    CHECK(!NativeExecutionConsumerProbe::exact(single_overflow.consumer()));
    seal(single_overflow);
    CHECK(single_overflow.bars.back().volume == std::ldexp(1.0, 63) + 2048);

    Host quiet;
    begin(quiet, 0.1);
    push(quiet, {0.1, 0.2});
    CHECK(quiet.stream_advance_time(180000));
    CHECK(quiet.bars.size() == 2);
    CHECK(quiet.bars[0].volume == 0.3);
    CHECK(quiet.bars[1].volume == 0.0);
    CHECK(quiet.bars[1].open == 100 && quiet.bars[1].close == 100);
}

void correctly_rounded_large_units() {
    uint64_t seed = 84729531;
    double divisor = 1.0;
    for (int places = 0; places <= 22; ++places, divisor *= 10.0) {
        const double step = 1.0 / divisor;
        for (int sample = 0; sample < 128; ++sample) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            const int64_t units = static_cast<int64_t>(seed & 0x7fffffffffffffffULL);
            const std::string decimal = std::to_string(units) + "e-" + std::to_string(places);
            CHECK(NativeExecutionConsumerProbe::decimal_sum(step, units)
                  == std::strtod(decimal.c_str(), nullptr));
        }
    }
    Host large;
    begin(large, 0.1);
    push(large, {450359962737049.6, 450359962737049.6, 0.1});
    seal(large);
    CHECK(large.bars.back().volume == std::strtod("900719925474099.3", nullptr));
    Host fine;
    begin(fine, 1e-22);
    push(fine, {1e-22, 2e-22});
    CHECK(NativeExecutionConsumerProbe::exact(fine.consumer()));
    seal(fine);
    CHECK(fine.bars.back().volume == std::strtod("3e-22", nullptr));
}

void preflight_is_atomic() {
    Host host;
    begin(host, 0.1);
    push(host, {0.1, 0.2});
    const auto hash = host.native_continuation_hash();
    const double maximum = std::numeric_limits<double>::max();
    const TradeTick invalid[] = {{60002, 3, 100, maximum}, {60003, 4, 100, maximum}};
    CHECK(!host.stream_push_ticks(invalid, 2));
    CHECK(host.native_continuation_hash() == hash);
    CHECK(host.current_partial_bar()->volume == 0.3);
    CHECK(host.stream_push_tick({60004, 3, 100, 0.1}));
    seal(host);
    CHECK(host.bars.back().volume == 0.4);

    Host across_inputs;
    begin(across_inputs, std::nullopt, "5");
    const TradeTick invalid_partial[] = {{60001, 1, 100, maximum}, {120001, 2, 100, maximum}};
    CHECK(!across_inputs.stream_push_ticks(invalid_partial, 2));
    CHECK(across_inputs.current_partial_bar() == std::nullopt);
}

bool same_bar(const Bar& actual, const Bar& expected) {
    return actual.timestamp == expected.timestamp && actual.open == expected.open
        && actual.high == expected.high && actual.low == expected.low
        && actual.close == expected.close && actual.volume == expected.volume;
}

void tick_bars_equal_decimal_oracle_and_replay() {
    std::vector<TradeTick> ticks;
    std::vector<Bar> oracle;
    uint64_t seed = 912781;
    for (int slot = 0; slot < 24; ++slot) {
        const int64_t timestamp = 60000 + slot * 60000;
        int64_t total_units = 0;
        Bar bar{0, 0, std::numeric_limits<double>::max(), 0, 0, timestamp};
        for (int print = 0; print < 37; ++print) {
            seed = seed * 1664525 + 1013904223;
            const int64_t units = static_cast<int64_t>(seed % 100000 + 1);
            const double price = 100 + static_cast<double>(seed % 100) / 100;
            total_units += units;
            if (print == 0) bar.open = price;
            bar.high = std::max(bar.high, price);
            bar.low = std::min(bar.low, price);
            bar.close = price;
            const std::string quantity = std::to_string(units) + "e-3";
            ticks.push_back({timestamp + print, ticks.size() + 1, price,
                             std::strtod(quantity.c_str(), nullptr)});
        }
        const std::string total = std::to_string(total_units) + "e-3";
        bar.volume = std::strtod(total.c_str(), nullptr);
        oracle.push_back(bar);
    }
    Host whole, chunked, restarted;
    begin(whole, 0.001);
    begin(chunked, 0.001);
    begin(restarted, 0.001);
    CHECK(whole.stream_push_ticks(ticks.data(), static_cast<int>(ticks.size())));
    for (std::size_t cursor = 0; cursor < ticks.size(); cursor += 17) {
        const int count = static_cast<int>(std::min<std::size_t>(17, ticks.size() - cursor));
        CHECK(chunked.stream_push_ticks(ticks.data() + cursor, count));
    }
    for (const auto& tick : ticks) CHECK(restarted.stream_push_tick(tick));
    CHECK(whole.native_continuation_hash() == chunked.native_continuation_hash());
    CHECK(whole.native_continuation_hash() == restarted.native_continuation_hash());
    for (auto* host : {&whole, &chunked, &restarted}) {
        CHECK(host->stream_advance_time(1500000));
        CHECK(host->bars.size() == oracle.size());
        for (std::size_t index = 0; index < oracle.size(); ++index)
            CHECK(same_bar(host->bars[index], oracle[index]));
    }
    CHECK(whole.native_continuation_hash() == chunked.native_continuation_hash());
    CHECK(whole.native_continuation_hash() == restarted.native_continuation_hash());

    Host partial;
    begin(partial, 0.1, "5");
    const TradeTick script_ticks[] = {{60001, 1, 100, 0.1}, {120001, 2, 101, 0.2}};
    CHECK(partial.stream_push_ticks(script_ticks, 2));
    CHECK(partial.partial_volume == 0.3);
    CHECK(partial.current_partial_bar()->volume == 0.3);
}
}

int main() {
    const auto run_case = [](const char* name, void (*test)()) {
        const int previous_failures = failures;
        test();
        std::printf("%s %s\n", failures == previous_failures ? "PASS" : "FAIL", name);
    };
    run_case("decimal and pinned 3423.186", small_decimal_and_pinned_volume);
    run_case("compensated off-grid and absent/nondecimal steps", compensated_fallbacks);
    run_case("int64 overflow and quiet carried bar", integer_overflow_and_quiet_slot);
    run_case("correct rounding through 22 decimal places", correctly_rounded_large_units);
    run_case("atomic forming and partial overflow preflight", preflight_is_atomic);
    run_case("tick bars equal decimal oracle and chunk/replay hashes", tick_bars_equal_decimal_oracle_and_replay);
    std::printf("test_native_tick_volume: %s (%d checks)\n", failures ? "FAIL" : "PASS", checks);
    return failures ? 1 : 0;
}
