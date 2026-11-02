#include <pineforge/series.hpp>
#include <pineforge/generic_matrix.hpp>
// Source detail helpers may already be declared by another source header.
namespace pineforge::source::detail {}
#include <pineforge/source/pine_ta_length.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace pineforge;

static void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

static std::uint64_t bits(double value) {
    std::uint64_t result;
    std::memcpy(&result, &value, sizeof result);
    return result;
}

static double sample(int i) {
    if (i % 97 == 0) return na<double>();
    if (i % 41 == 0) return -0.0;
    return static_cast<double>((i * 37) % 211 - 105) / 8.0;
}

static void report(const char* name, WindowCacheCapacity capacity, const WindowCacheStats& stats) {
    std::printf("%s pages=%zu payload=%zu metadata=%zu workspace=%zu "
                "hits=%llu misses=%llu evictions=%llu bypass=%llu "
                "reads=%llu read_bytes=%llu writes=%llu write_bytes=%llu "
                "inner_read_attempts=%llu inner_write_attempts=%llu\n",
                name, capacity.pages, stats.resident_payload_bytes,
                stats.resident_metadata_bytes, stats.workspace_bytes,
                static_cast<unsigned long long>(stats.hits),
                static_cast<unsigned long long>(stats.misses),
                static_cast<unsigned long long>(stats.evictions),
                static_cast<unsigned long long>(stats.bypass_reads),
                static_cast<unsigned long long>(stats.backing_reads),
                static_cast<unsigned long long>(stats.backing_read_bytes),
                static_cast<unsigned long long>(stats.backing_writes),
                static_cast<unsigned long long>(stats.backing_write_bytes),
                static_cast<unsigned long long>(stats.backing_read_attempts),
                static_cast<unsigned long long>(stats.backing_write_attempts));
}

static void cold_pages() {
    constexpr std::size_t chunk = detail::WindowValues<double>::kChunk;
    detail::WindowValues<double> values(3 * chunk + 17, na<double>(), {1});
    values.write(7, 42.25);
    values.write(chunk + 7, -0.0);
    const auto before = values.cache_stats();
    require(bits(values.read(7)) == bits(42.25), "first page reads its backing value");
    require(bits(values.read(7)) == bits(42.25), "hot page reads its backing value");
    require(bits(values.read(chunk + 7)) == bits(-0.0), "collision preserves signed zero");
    require(bits(values.read(7)) == bits(42.25), "evicted page remains readable");
    const auto after = values.cache_stats();
    require(after.backing_reads - before.backing_reads == 3,
            "three cold pages perform three backing reads");
    require(after.backing_read_attempts - before.backing_read_attempts == 3,
            "cold page proof comes from the inner backing primitive");
    require(after.hits - before.hits == 1, "only the repeated hot read hits");
    require(after.evictions - before.evictions == 2, "colliding pages evict twice");
    require(after.resident_payload_bytes == chunk * sizeof(double),
            "one chunk bounds resident numeric payload");
    const auto shared = values.read_cache_stats();
    const auto policy = values.read_cache_policy();
    require(shared.get_calls == 4 && shared.hits == 1 && shared.misses == 3,
            "numeric page reads traverse the shared cache");
    require(policy.capacity_bytes == detail::ReadThroughCache::line_bytes(chunk * sizeof(double))
                && policy.page_size == chunk * sizeof(double)
                && policy.replacement == detail::ReadThroughCacheReplacement::direct_mapped
                && policy.write_policy == detail::ReadThroughCacheWritePolicy::write_through,
            "one-page policy includes the shared payload and complete metadata");
    require(shared.current_bytes == policy.capacity_bytes
                && shared.current_payload_bytes == after.resident_payload_bytes,
            "shared cache is the only numeric payload cache");

    detail::WindowValues<double> bypass(3 * chunk + 17, na<double>(), {0});
    bypass.write(7, 42.25);
    const auto direct_before = bypass.cache_stats();
    for (int i = 0; i != 3; ++i)
        require(bits(bypass.read(7)) == bits(42.25), "disabled cache reads backing");
    const auto direct_after = bypass.cache_stats();
    require(direct_after.backing_reads - direct_before.backing_reads == 3,
            "bypass performs every backing read");
    require(direct_after.backing_read_attempts - direct_before.backing_read_attempts == 3,
            "each bypass read reaches the inner primitive");
    require(direct_after.resident_payload_bytes == 0, "bypass allocates no cache payload");

    // Partial tail pages and writes through a previously hot page.
    values.write(3 * chunk + 16, -31.0);
    require(values.read(3 * chunk + 16) == -31.0, "partial final page reads exact extent");
    values.write(3 * chunk + 16, 51.0);
    require(values.read(3 * chunk + 16) == 51.0, "hot tail write invalidates old bytes");
    auto checkpoint = values;
    values.write(3 * chunk + 16, 71.0);
    require(checkpoint.read(3 * chunk + 16) == 51.0, "partial tail checkpoint is immutable");
    values = checkpoint;
    require(values.read(3 * chunk + 16) == 51.0, "assignment discards previous cache generation");
    values.reset(3 * chunk + 17, -7.0);
    values.write(3, 5.0);
    require(values.read(3 * chunk + 16) == -7.0, "reset never serves an earlier generation");
}

static void moved_cache_owners() {
    using Values = detail::WindowValues<double>;
    constexpr auto count = 3 * Values::kChunk + 17;
    std::optional<Values> moved;
    {
        Values original(count, -3.0, {1});
        original.write(3, 19.0);
        original.write(Values::kChunk + 3, 29.0);
        require(original.read(3) == 19.0, "warm original before moving its owner");
        moved.emplace(std::move(original));
        original.reset(count, 211.0);
        original.write(3, 211.0);
    }
    require(moved->read_cache_stats().get_calls == 0, "move reconstructs cache control lazily");
    const auto before = moved->cache_stats().backing_read_attempts;
    require(moved->read(3) == 19.0 && moved->read(Values::kChunk + 3) == 29.0,
            "moved cache reads its own backing after the old owner is destroyed");
    require(moved->cache_stats().backing_read_attempts - before == 2,
            "move's cold reads are observed at the inner backing");
    Values assigned(count, 41.0, {1});
    assigned.write(3, 41.0);
    require(assigned.read(3) == 41.0, "warm move-assignment destination");
    assigned = std::move(*moved);
    moved.reset();
    require(assigned.read(Values::kChunk + 3) == 29.0 && assigned.read(3) == 19.0,
            "move assignment replaces both callback context and warm destination tags");
    auto checkpoint = assigned;
    assigned.write(3, 53.0);
    require(checkpoint.read(3) == 19.0 && assigned.read(3) == 53.0,
            "moved owner retains checkpoint isolation on the next write");
}

static void typed_series(WindowCacheCapacity capacity) {
    Series<bool> flags(2057, capacity);
    Series<std::int64_t> integers(2057, capacity);
    Series<std::string> strings(3, capacity);
    for (int i = 0; i != 2060; ++i) {
        flags.push(i % 3 == 0);
        integers.push(i);
    }
    for (int offset : {0, 1024, 2056}) {
        require(flags[offset] == ((2059 - offset) % 3 == 0), "boolean numeric pages");
        require(integers[offset] == 2059 - offset, "integer numeric pages");
    }
    strings.push("old");
    auto string_copy = strings;
    strings.update("new");
    require(string_copy[0] == "old" && strings[0] == "new", "owning typed values keep snapshots");
    using Matrix = PineGenericMatrix<int>;
    Series<Matrix> handles(3, capacity);
    auto matrix = Matrix::new_(1, 1, 17);
    handles.push(matrix);
    auto checkpoint = handles;
    matrix.set(0, 0, 23);
    require(handles[0].get(0, 0) == 23 && checkpoint[0].get(0, 0) == 23,
            "handle-valued Series preserve matrix identity through checkpoints");
    handles.update(Matrix::new_(1, 1, 41));
    require(checkpoint[0].get(0, 0) == 23, "slot replacement keeps old matrix handle");
}

static void scope_latching() {
    const auto previous = window_cache_capacity();
    std::optional<Series<double>> latched;
    {
        WindowCacheScope one({1});
        latched.emplace(2057);
        {
            WindowCacheScope bypass({0});
            Series<double> inner;
            require(inner.cache_capacity().pages == 0, "nested scope supplies constructor default");
            require(latched->cache_capacity().pages == 1, "new scope cannot resize an existing window");
        }
        require(window_cache_capacity().pages == 1, "nested scope restores its caller");
    }
    require(window_cache_capacity().pages == previous.pages, "outer scope restores its caller");
    latched->push(1.0);
    require(latched->cache_stats().scratch_backed, "deferred allocation keeps constructor policy");
}

static void series_boundaries(WindowCacheCapacity capacity) {
    constexpr int length = 2057;
    Series<double> series(length, capacity);
    std::deque<double> expected;
    for (int i = 0; i != 2 * length + 9; ++i) {
        double value = sample(i);
        series.push(value);
        expected.push_front(value);
        if (expected.size() > length) expected.pop_back();
        if (i % 19 == 0) {
            value = sample(i + 3);
            series.update(value);
            expected.front() = value;
        }
        require(series.size() == static_cast<int>(expected.size()), "semantic Series size");
        for (int offset : {0, 1, 1023, 1024, 2048, length - 1, length}) {
            const double want = offset < static_cast<int>(expected.size())
                                    ? expected[static_cast<std::size_t>(offset)]
                                    : na<double>();
            require(bits(series[offset]) == bits(want), "Series read matches owning deque");
        }
        require(is_na(series[-1]), "negative history stays na");
    }
    auto checkpoint = series;
    const double checkpoint_value = checkpoint[0];
    series.update(123.0);
    require(bits(checkpoint[0]) == bits(checkpoint_value), "Series checkpoint is independent");
    report("Series", capacity, series.cache_stats());
    series.clear();
    require(series.size() == 0 && is_na(series[0]), "clear retires logical history");
    series.update(-0.0);
    require(series.size() == 1 && bits(series[0]) == bits(-0.0), "update seeds after clear");

    Series<double> disabled(0, capacity);
    disabled.push(3.0);
    disabled.update(4.0);
    require(disabled.size() == 0 && is_na(disabled[0]), "zero logical capacity drops writes");

    DynamicRingBuffer<double> ring(3077, capacity);
    require(ring.cache_stats().resident_payload_bytes == 0, "empty ring stays lazy");
    ring.resize(4099);
    require(ring.cache_stats().resident_payload_bytes == 0, "empty resize stays lazy");
    for (int i = 0; i != 5000; ++i) ring.push_front(sample(i));
    ring.resize(1031);
    for (int i = 0; i != 1031; ++i)
        require(bits(ring[static_cast<std::size_t>(i)]) == bits(sample(4999 - i)),
                "resize preserves newest values in order");
    ring.resize(5001);
    require(ring.size() == 1031 && is_na(ring[1031]), "growth does not invent history");
}

// An owning chronological model, independent of storage chunks and cache tags.
static void held_history(WindowCacheCapacity capacity) {
    constexpr std::size_t keep = 15001;
    source::HeldHistory history(capacity);
    std::deque<double> expected;
    long long first = 0;
    long long last = -1;
    for (long long bar = 0; bar != 19000; ++bar) {
        if (bar % 13 != 3 && bar != 18999) continue;
        const double value = sample(static_cast<int>(bar));
        if (expected.empty()) first = bar;
        else
            for (long long skipped = last + 1; skipped < bar; ++skipped)
                expected.push_back(expected.back());
        expected.push_back(value);
        last = bar;
        while (expected.size() > source::HeldHistory::kChunk &&
               expected.size() - source::HeldHistory::kChunk >= keep) {
            for (std::size_t i = 0; i != source::HeldHistory::kChunk; ++i)
                expected.pop_front();
            first += source::HeldHistory::kChunk;
        }
        history.write(bar, value, keep);
        require(history.first_bar() == first && history.last_bar() == last,
                "HeldHistory chunk retirement matches legacy boundaries");
        for (std::size_t i : {std::size_t(0), expected.size() / 2, expected.size() - 1})
            require(bits(history.at(first + static_cast<long long>(i))) == bits(expected[i]),
                    "HeldHistory holds skipped bars and rereads old chunks");
        if (capacity.pages >= 16) {
            const auto chunks = expected.size() / source::HeldHistory::kChunk +
                                (expected.size() % source::HeldHistory::kChunk != 0);
            require(history.cache_stats().resident_payload_bytes ==
                        chunks * source::HeldHistory::kChunk * sizeof(double),
                    "default history releases semantically retired chunks");
        }
    }
    auto checkpoint = history;
    history.write(last, 999.0, keep);
    require(bits(checkpoint.at(last)) == bits(expected.back()), "history checkpoint keeps old tick");
    history.write(last - 1, 123.0, keep);
    require(bits(history.at(last)) == bits(999.0), "earlier history is never rewritten");
    history.write(last + static_cast<long long>(keep) + 7, 77.0, keep);
    const long long new_last = history.last_bar();
    require(history.first_bar() == new_last - static_cast<long long>(keep),
            "long gap restarts at readable history");
    require(bits(history.at(history.first_bar())) == bits(999.0),
            "long gap preserves the held source");
    require(bits(history.at(new_last)) == bits(77.0), "long gap stores the new source");
    if (capacity.pages == 1)
        require(history.cache_stats().backing_reads != 0, "tiny history exercises backing I/O");
    report("HeldHistory", capacity, history.cache_stats());
}

static void extrema_rescans(WindowCacheCapacity capacity) {
    constexpr int length = 2 * static_cast<int>(detail::WindowValues<double>::kChunk) + 9;
    ta::ExtremeRing high(length, capacity), low(length, capacity);
    std::deque<double> high_values, low_values;
    // No na or better extremum can reset the cache before expiry. Continue
    // through three rescans, including the length+1 physical ring wrap.
    for (int bar = 0; bar != length + 3; ++bar) {
        ta::BarContextScope context(bar, 0);
        const double value = static_cast<double>(length - bar) / 8.0;
        high_values.push_back(value);
        low_values.push_back(-value);
        if (high_values.size() > static_cast<std::size_t>(length)) {
            high_values.pop_front();
            low_values.pop_front();
        }
        const auto high_before = high.cache_stats();
        const auto low_before = low.cache_stats();
        const auto hi = high.update(value, true, true);
        const auto lo = low.update(-value, true, false);
        ta::ExtremeRing::Result expected_high{na<double>(), 0}, expected_low{na<double>(), 0};
        if (high_values.size() == static_cast<std::size_t>(length)) {
            const auto maximum = std::max_element(high_values.begin(), high_values.end());
            const auto minimum = std::min_element(low_values.begin(), low_values.end());
            expected_high = {*maximum, static_cast<int>(std::distance(maximum, high_values.end())) - 1};
            expected_low = {*minimum, static_cast<int>(std::distance(minimum, low_values.end())) - 1};
        }
        require(bits(hi.value) == bits(expected_high.value)
                    && hi.bars_back == expected_high.bars_back,
                "expiry and wrap highest match the independent deque at every bar");
        require(bits(lo.value) == bits(expected_low.value)
                    && lo.bars_back == expected_low.bars_back,
                "expiry and wrap lowest match the independent deque at every bar");
        if (capacity.pages <= 2 && bar >= length) {
            const auto high_after = high.cache_stats();
            const auto low_after = low.cache_stats();
            require(high_after.backing_read_attempts > high_before.backing_read_attempts,
                    "tiny extrema sweep proves inner backing reads");
            require(low_after.backing_read_attempts > low_before.backing_read_attempts,
                    "tiny lowest sweep proves inner backing reads");
            require(high_after.backing_reads > high_before.backing_reads
                        && high_after.backing_read_bytes > high_before.backing_read_bytes
                        && low_after.backing_reads > low_before.backing_reads
                        && low_after.backing_read_bytes > low_before.backing_read_bytes,
                    "each tiny extrema rescan completes real backing transfers");
        }
    }
    report("ExtremeRingRescanHigh", capacity, high.cache_stats());
    report("ExtremeRingRescanLow", capacity, low.cache_stats());
}

struct Tape {
    std::vector<std::uint64_t> values;
    void add(double value) { values.push_back(bits(value)); }
};

static Tape ta_tape(WindowCacheCapacity capacity) {
    WindowCacheScope scope(capacity);
    constexpr int length = 2057;
    ta::ExtremeRing high(length), low(length);
    ta::WMA wma(length);
    source::SeriesHighest series_high;
    source::SeriesLowest series_low;
    source::SeriesHighestBars series_high_bars;
    source::SeriesLowestBars series_low_bars;
    Tape tape;
    for (int bar = 0; bar != 18050; ++bar) {
        ta::BarContextScope context(bar, 0);
        const double value = sample(bar);
        if (bar % 7 == 3 || bar % 29 == 5) {
            for (bool advance : {true, false, false}) {
                const double tick = advance ? value : sample(bar + 2);
                const auto hi = high.update(tick, advance, true);
                const auto lo = low.update(tick, advance, false);
                tape.add(hi.value);
                tape.add(hi.bars_back);
                tape.add(lo.value);
                tape.add(lo.bars_back);
                tape.add(advance ? wma.compute(tick) : wma.recompute(tick));
            }
        }
        if (bar % 17 == 4 || bar == 18049) {
            const double len = bar < 15500 ? 1 + (bar * 13) % 4990 : 15001;
            tape.add(series_high.compute(value, len));
            tape.add(series_low.compute(value, len));
            tape.add(series_high_bars.compute(value, len));
            tape.add(series_low_bars.compute(value, len));
            tape.add(series_high.recompute(sample(bar + 5), len));
            tape.add(series_low.recompute(sample(bar + 5), len));
            tape.add(series_high_bars.recompute(sample(bar + 5), len));
            tape.add(series_low_bars.recompute(sample(bar + 5), len));
        }
    }
    // No-context cadence, same-bar replacement, and copied call-site state.
    ta::bar_context() = {};
    ta::ExtremeRing independent_high(17);
    source::SeriesHighest independent_series;
    for (int i = 0; i != 32; ++i) {
        tape.add(independent_high.update(sample(i), true, true).value);
        tape.add(independent_series.compute(sample(i), 17));
    }
    auto high_copy = independent_high;
    auto series_copy = independent_series;
    for (int i = 32; i != 64; ++i) {
        const double value = sample(i);
        tape.add(high_copy.update(value, true, true).value);
        tape.add(high_copy.update(value + 1.0, false, true).value);
        tape.add(series_copy.compute(value, 17));
        tape.add(series_copy.recompute(value + 1.0, 17));
    }
    report("ExtremeRing", capacity, high.cache_stats());
    report("WMA", capacity, wma.cache_stats());
    if (capacity.pages <= 2) {
        require(wma.cache_stats().backing_read_attempts != 0,
                "tiny weighted sweep proves inner backing reads");
        if (capacity.pages != 0)
            require(wma.cache_stats().evictions != 0, "weighted window exceeds one cache chunk");
    }
    return tape;
}

static constexpr std::size_t occurrences(std::size_t count, std::size_t period,
                                         std::size_t first) {
    return count > first ? 1 + (count - 1 - first) / period : 0;
}

static void semantic_caps(WindowCacheCapacity capacity) {
    WindowCacheScope scope(capacity);
    ta::BarContextScope context(16000, 0);
    source::SeriesHighest highest;
    require(bits(highest.compute(7.0, 15001)) == bits(7.0), "15000 bars back stays valid");
    for (double length : {15002.0, 0.0, -3.0, na<double>()}) {
        bool failed = false;
        try {
            (void)highest.compute(7.0, length);
        } catch (const std::runtime_error& error) {
            const auto* info = dynamic_cast<const RunFailureInfo*>(&error);
            require(info != nullptr, "semantic failures keep typed metadata");
            require(info->run_failure_code() == (length == 15002.0
                        ? RunFailureCode::pine_runtime_limit
                        : RunFailureCode::pine_invalid_argument),
                    "cache size preserves semantic failure code");
            if (length == 15002.0)
                require(std::string(error.what()).find("limit is 15000 bars") != std::string::npos,
                        "max_bars_back refusal text is unchanged");
            failed = true;
        }
        require(failed, "invalid semantic length still refuses");
    }
}

int main() {
    cold_pages();
    moved_cache_owners();
    scope_latching();
    for (WindowCacheCapacity capacity : {WindowCacheCapacity{0}, WindowCacheCapacity{1},
                                         WindowCacheCapacity{2},
                                         WindowCacheCapacity{64}, WindowCacheCapacity{}}) {
        series_boundaries(capacity);
        typed_series(capacity);
        held_history(capacity);
        semantic_caps(capacity);
        extrema_rescans(capacity);
    }
    const auto large = ta_tape({});
    // The fixed-window cadence intersects at 150 modulo 7*29; the final
    // series-length call is outside its ordinary modulo-17 cadence.
    constexpr auto fixed_calls = occurrences(18050, 7, 3) + occurrences(18050, 29, 5)
                                 - occurrences(18050, 7 * 29, 150);
    constexpr auto series_calls = occurrences(18050, 17, 4) + 1;
    require(large.values.size() == 15 * fixed_calls + 8 * series_calls + 6 * 32,
            "TA tape contains every value, offset and replacement tick");
    require(ta_tape({1}).values == large.values, "one-chunk TA tape equals default bit for bit");
    require(ta_tape({2}).values == large.values, "two-chunk TA tape equals default bit for bit");
    require(ta_tape({0}).values == large.values, "bypass TA tape equals default bit for bit");
    require(ta_tape({64}).values == large.values, "64-chunk TA tape equals default bit for bit");
    std::puts("TA history cache: all values, offsets, caps, and checkpoints verified");
}
