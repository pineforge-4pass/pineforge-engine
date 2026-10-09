// Shared by the test_outputs_* rows: a bare NativeStrategyHost that records
// outputs, synthetic bars, and a reader that sees the record only through the
// strategy_outputs_* C exports (include/pineforge/pineforge.h, group
// pf_outputs). The host states its own rows, as the recorder requires: it
// opens one row per calculation whose script bar opens after the last row it
// opened, and skips a calculation that does not (the rule the Pine host keeps
// for a stream's stale carried callback). Source-free.
#pragma once

#include <pineforge/pineforge.h>
#include <pineforge/native_host.hpp>
#include <pineforge/ta.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace outputs_test {

using namespace pineforge;

inline int passed = 0;
inline int failed = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (cond) {                                                                   \
            ++outputs_test::passed;                                                   \
        } else {                                                                      \
            ++outputs_test::failed;                                                   \
            std::fprintf(stderr, "CHECK FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                             \
    } while (0)

inline int finish(const char* name) {
    std::printf("%s: %d passed, %d failed\n", name, passed, failed);
    return failed == 0 ? 0 : 1;
}

constexpr std::int64_t kT0 = 1704067200000LL;  // 2024-01-01 00:00 UTC
constexpr std::int64_t kMinute = 60000;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

inline NativeRunSpec make_spec(const char* key, std::uint64_t run_number,
                               const char* input_tf = "5", const char* script_tf = "5") {
    NativeRunSpec spec;
    spec.identity.session_key = key;
    spec.identity.run_number = run_number;
    spec.input_tf = input_tf;
    spec.script_tf = script_tf;
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::Percent;
    spec.fee_value = 0.0;
    return spec;
}

// n bars `step_ms` apart from `t0`: a deterministic walk with up and down bars.
inline std::vector<Bar> make_bars(int n, std::int64_t step_ms, std::int64_t t0 = kT0,
                                  double base = 100.0) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(n));
    double price = base;
    for (int i = 0; i < n; ++i) {
        const double drift = ((i * 7) % 5) - 2.0;       // -2, 0, 2, -1, 1, ...
        const double open = price;
        const double close = price + drift * 0.5 + ((i % 3) == 0 ? 0.25 : -0.25);
        const double high = std::max(open, close) + 0.75;
        const double low = std::min(open, close) - 0.5;
        bars.push_back({open, high, low, close, 10.0 + i, t0 + i * step_ms});
        price = close;
    }
    return bars;
}

class OutputsHost;
using Script = std::function<void(OutputsHost&, const Bar&, const NativeDecisionContext&)>;

// The C++ host: the protected recorder calls are forwarded publicly so each
// test states its script as a lambda.
class OutputsHost : public NativeStrategyHost {
public:
    struct Shape {
        int slots = 0;
        int outputs = 0;
        int constants = 0;
        bool declare = true;
    };

    explicit OutputsHost(Shape shape) {
        if (shape.declare) declare_outputs(shape.slots, shape.outputs, shape.constants);
    }

    Script script;
    std::function<void(OutputsHost&)> on_begin;
    bool begin_run = true;            // call output_run_begin() in on_native_run_begin
    bool skip_stale = true;           // the Pine rule: no row for a stale calculation
    int calculations = 0;             // every on_native_bar the kernel dispatched
    int published = 0;                // the calculations this host published
    int stale = 0;                    // the calculations it skipped
    std::int64_t last_open = std::numeric_limits<std::int64_t>::min();

    pf_strategy_t handle() { return static_cast<pf_strategy_t>(static_cast<BacktestEngine*>(this)); }

    bool recording() const { return outputs_enabled_; }
    void declare(int slots, int outputs, int constants = 0) { declare_outputs(slots, outputs, constants); }
    void run_begin() { output_run_begin(); }
    void bar(std::int64_t open_ms, std::int64_t close_ms) { output_bar(open_ms, close_ms); }
    void value(int slot, double v) { output_value(slot, v); }
    void event(int output, double v) { output_event(output, v); }
    void event(int output, double v, const std::string& message) { output_event(output, v, message); }
    void constant(int index, double v) { output_constant(index, v); }

    void on_native_run_begin() override {
        calculations = published = stale = 0;
        last_open = std::numeric_limits<std::int64_t>::min();
        if (begin_run) output_run_begin();
        if (on_begin) on_begin(*this);
    }

    void on_native_bar(const Bar& b, const NativeDecisionContext& context) override {
        ++calculations;
        if (skip_stale && context.script_bar_open_ms <= last_open) {
            ++stale;
            return;
        }
        last_open = context.script_bar_open_ms;
        ++published;
        // NativeDecisionContext::script_bar_open_ms is the open a C++ host
        // states; the bar's close is its script interval's end.
        if (outputs_enabled_) output_bar(context.script_bar_open_ms, context.script_interval.next_period_open_ms);
        if (script) script(*this, b, context);
    }
};

struct EventRecord {
    std::uint32_t struct_version = 0;
    std::uint32_t size = 0;
    std::uint64_t sequence = 0;
    std::int32_t output_index = 0;
    std::int32_t bar_index = 0;
    std::int64_t bar_open_ms = 0;
    std::int64_t bar_close_ms = 0;
    std::uint32_t ordinal_in_bar = 0;
    std::uint32_t phase = 0;
    std::uint32_t confirmed = 0;
    double value = 0.0;
    std::uint64_t message_hash64 = 0;
    bool has_message = false;
    std::string message;
};

struct Record {
    int series_count = 0;
    std::int64_t bars = 0;
    std::vector<std::int64_t> open_ms, close_ms;
    std::vector<std::vector<double>> series;  // [slot][row]
    std::vector<EventRecord> events;
    std::vector<double> constants;
};

inline EventRecord read_event(pf_strategy_t s, int index) {
    pf_output_event_v1_t raw;
    std::memset(&raw, 0, sizeof(raw));
    EventRecord out;
    if (strategy_outputs_event_get(s, index, &raw, sizeof(raw)) != 0) {
        out.sequence = 0;
        return out;
    }
    out.struct_version = raw.struct_version;
    out.size = raw.size;
    out.sequence = raw.sequence;
    out.output_index = raw.output_index;
    out.bar_index = raw.bar_index;
    out.bar_open_ms = raw.bar_open_ms;
    out.bar_close_ms = raw.bar_close_ms;
    out.ordinal_in_bar = raw.ordinal_in_bar;
    out.phase = raw.phase;
    out.confirmed = raw.confirmed;
    out.value = raw.value;
    out.message_hash64 = raw.message_hash64;
    out.has_message = raw.message != nullptr;
    if (raw.message) out.message = raw.message;
    return out;
}

// The events queued now, in queue order.
inline std::vector<EventRecord> read_events(pf_strategy_t s) {
    std::vector<EventRecord> out;
    const int n = strategy_outputs_events_len(s);
    for (int i = 0; i < n; ++i) out.push_back(read_event(s, i));
    return out;
}

inline Record read_record(pf_strategy_t s) {
    Record r;
    r.series_count = strategy_outputs_series_count(s);
    r.bars = strategy_outputs_bars_len(s);
    if (r.bars > 0) {
        r.open_ms.assign(static_cast<std::size_t>(r.bars), 0);
        r.close_ms.assign(static_cast<std::size_t>(r.bars), 0);
        std::int64_t written = -1;
        if (strategy_outputs_bar_times_copy(s, 0, r.open_ms.data(), r.close_ms.data(), r.bars,
                                            &written) != 0
            || written != r.bars) {
            r.open_ms.clear();
            r.close_ms.clear();
        }
    }
    for (int slot = 0; slot < r.series_count; ++slot) {
        std::vector<double> values(static_cast<std::size_t>(r.bars), 0.0);
        std::int64_t written = -1;
        if (strategy_outputs_series_copy(s, slot, 0, values.data(), r.bars, &written) != 0
            || written != r.bars) {
            values.clear();
        }
        r.series.push_back(std::move(values));
    }
    r.events = read_events(s);
    const int n = strategy_outputs_constants_copy(s, nullptr, 0);
    if (n > 0) {
        r.constants.assign(static_cast<std::size_t>(n), 0.0);
        strategy_outputs_constants_copy(s, r.constants.data(), n);
    }
    return r;
}

inline std::uint64_t bits(double v) {
    std::uint64_t out;
    std::memcpy(&out, &v, sizeof(out));
    return out;
}

// Bit equality, every NaN equal to every NaN.
inline bool same_value(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    return bits(a) == bits(b);
}

inline bool same_values(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!same_value(a[i], b[i])) return false;
    return true;
}

// Every field but `phase` when `with_phase` is false.
inline bool same_event(const EventRecord& a, const EventRecord& b, bool with_phase) {
    return a.struct_version == b.struct_version && a.size == b.size
        && a.sequence == b.sequence && a.output_index == b.output_index
        && a.bar_index == b.bar_index && a.bar_open_ms == b.bar_open_ms
        && a.bar_close_ms == b.bar_close_ms && a.ordinal_in_bar == b.ordinal_in_bar
        && (!with_phase || a.phase == b.phase) && a.confirmed == b.confirmed
        && same_value(a.value, b.value) && a.message_hash64 == b.message_hash64
        && a.has_message == b.has_message && a.message == b.message;
}

inline bool same_events(const std::vector<EventRecord>& a, const std::vector<EventRecord>& b,
                        bool with_phase) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!same_event(a[i], b[i], with_phase)) return false;
    return true;
}

// FNV-1a 64 of a message's bytes, the recorder's message_hash64.
inline std::uint64_t fnv1a64(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

// A colour as an rgba-u32 slot value: 0xAARRGGBB -> 0xRRGGBBAA as a double.
inline double rgba(std::uint32_t argb) {
    return static_cast<double>((argb << 8) | (argb >> 24));
}

}  // namespace outputs_test
