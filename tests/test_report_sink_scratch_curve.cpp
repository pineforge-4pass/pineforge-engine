// CR-150 L6b-4, fix rounds 1-2: the engine WRITE side and sink removal.
//
// The accepted library (src/report_scratch.*) is proved by test_report_scratch;
// this fixture proves its engine integration. DESIGN 3.2/3.8: with a
// sink installed, record_equity_point releases the previously held point to the
// E record AND to the engine's scratch curve, and BacktestEngine::equity_curve_
// keeps only the held tail. Real native host fixture, the form of
// tests/test_report_sink_l6b3.cpp (NativeStrategyHost, set_report_sink_v1,
// KernelRecorded). Every expectation comes from the in-memory sink-off run of
// the same tape (the oracle) or from an index formula, never from output.
//   (a) equity_curve_ stays bounded with a sink on: a short tape and a tape four
//       times longer (both past one scratch batch) run on fresh hosts; the sizes
//       and capacities the fixture observes in every calculation and after the
//       run and each fill must not depend on the script bars. The sink-off pair
//       is the positive control: its curve grows with them.
//   (b) the scratch curve holds EVERY point the E records carry, bit for bit the
//       oracle's: before fill (the held tail is the one point not yet released),
//       after fill (the last E record is the presented copy of the held point,
//       edited by the fixture's hook in two cases; the engine's held point and
//       the scratch prefix stay the execution values), after a second fill
//       (nothing appended), with the first observation only after the fill, at
//       the first mid-run flush (batch + 2 bars) and on a reused handle (a
//       shorter tape starts a new curve).
//   (c) sink-off behaviour is unchanged: one point per script bar labelled with
//       its open, the whole curve published, no scratch owner or file, the same
//       on a handle whose sink was removed before running; a replaced sink keeps
//       the completed run's report computable;
//       sink-on report, hash and run facts equal the sink-off ones.
//   (d) a scratch_dir the engine cannot use fails the run with report_sink_failed,
//       never a silent RAM fallback.
//   (e) removal after an ordinary or observer-only run refuses with -3 and
//       retains the scratch-backed summary. On short and 4x tapes, engine
//       size/capacity and counted report-array peak/capacity stay at the same
//       tape's held-tail bound, including temporary allocations during removal
//       and repeated fills. The sink-off oracle is the growing positive control.
//
// The scratch curve is observed through the protected accessor
// BacktestEngine::report_sink_equity_scratch_region() (include/pineforge/engine.hpp):
// the ordinary sink's execution prefix, the points released to E and scratch,
// excluding the held point even after fill; {-1, 0, 0} without an ordinary
// scratch owner. Total history is that region plus last_equity_point().
//
// Fail-before (not run here; the lab records the diagnostic): on 85c6c9a253 the
// accessor does not exist, so the first diagnostic is the compile error at
// CurveProbe::probe_scratch(). With it stubbed to an empty region, the first
// failing assertion is (a).
// Fix2 fail-before on 6f036c8ff5 (not run here): the first attempted removal
// restores the whole engine curve, failing (e)'s size/capacity sample before
// the status assertion; an observer also allocates a full captured array.
#include <pineforge/execution_observer.h>
#include <pineforge/metrics_stream.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/report_sink.h>
#include <pineforge/report_sink_writer.hpp>
#include <pineforge/run_failure.hpp>

#include "../src/report_scratch.hpp"
#include "scratch_descriptor_fixture.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v19 {
int native_set_execution_observer_v1(BacktestEngine*, const pf_execution_observer_v1*) noexcept;
}
}

// Count actual new[] storage, including arrays freed before fill_report
// returns. In the flat memory cases there are no trades, outputs, security
// diagnostics, trace or hash recording: the report/capture curve is the only
// array. Scalar new (vectors and the fixed pread window) remains separate.
// Every array allocation/deallocation form uses the matched scalar aligned
// allocator, including sized/nothrow forms, so sanitizers see one allocator.
namespace report_arrays {

struct Counts {
    std::size_t live_bytes = 0;
    std::size_t peak_bytes = 0;
    std::size_t capacity_bytes = 0;
    std::size_t allocations = 0;
};

Counts* active = nullptr;

struct Header {
    void* base;
    std::size_t size;
    std::size_t alignment;
    Counts* owner;
};

void* take(std::size_t size, std::size_t alignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
    alignment = std::max(alignment, std::size_t(__STDCPP_DEFAULT_NEW_ALIGNMENT__));
    const std::size_t prefix = (sizeof(Header) + alignment - 1) / alignment * alignment;
    if (size > std::numeric_limits<std::size_t>::max() - prefix) throw std::bad_alloc();
    void* base = ::operator new(prefix + size, std::align_val_t(alignment));
    auto* block = static_cast<unsigned char*>(base) + prefix;
    const Header header{base, size, alignment, active};
    std::memcpy(block - sizeof(Header), &header, sizeof header);
    if (active) {
        active->live_bytes += size;
        active->peak_bytes = std::max(active->peak_bytes, active->live_bytes);
        active->capacity_bytes = std::max(active->capacity_bytes, size);
        ++active->allocations;
    }
    return block;
}

void give(void* memory) noexcept {
    if (!memory) return;
    Header header{};
    std::memcpy(&header, static_cast<unsigned char*>(memory) - sizeof(Header), sizeof header);
    if (header.owner) header.owner->live_bytes -= header.size;
    ::operator delete(header.base, std::align_val_t(header.alignment));
}

void* take_nothrow(std::size_t size,
                   std::size_t alignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__) noexcept {
    try {
        return take(size, alignment);
    } catch (...) {
        return nullptr;
    }
}

struct Scope {
    Counts* previous;
    explicit Scope(Counts& counts) : previous(active) { active = &counts; }
    ~Scope() { active = previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

}  // namespace report_arrays

void* operator new[](std::size_t size) { return report_arrays::take(size); }
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return report_arrays::take(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return report_arrays::take_nothrow(size);
}
void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    return report_arrays::take_nothrow(size, static_cast<std::size_t>(alignment));
}
void operator delete[](void* memory) noexcept { report_arrays::give(memory); }
void operator delete[](void* memory, std::size_t) noexcept { report_arrays::give(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { report_arrays::give(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
    report_arrays::give(memory);
}
void operator delete[](void* memory, const std::nothrow_t&) noexcept { report_arrays::give(memory); }
void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept {
    report_arrays::give(memory);
}

namespace {

using pineforge::BacktestEngine;
using pineforge::Bar;
using pineforge::NativeDecisionContext;
using pineforge::NativeReportPolicy;
using pineforge::NativeRunSpec;
using pineforge::NativeSetupStatus;
using pineforge::NativeStrategyHost;
using pineforge::ReportC;
namespace native_order = pineforge::native_order;
namespace rs = pineforge::report_sink;

constexpr std::int64_t start_ms = INT64_C(1736121600000);
constexpr std::int64_t step_ms = 60000;
constexpr double initial_capital = 10000.0;
// One scratch batch. A tape of more than batch + 1 bars releases more than one
// batch of points, so the engine's own writer flushes a full batch mid-run.
constexpr std::int64_t batch = static_cast<std::int64_t>(rs::kScratchWindowPoints);

std::string scenario = "setup";

[[noreturn]] void fail_test(const std::string& message) {
    throw std::runtime_error(scenario + ": " + message);
}

void require(bool condition, const std::string& message) {
    if (!condition) fail_test(message);
}

bool same_point(const pf_equity_point_t& actual, const pf_equity_point_t& expected) {
    return std::memcmp(&actual, &expected, sizeof actual) == 0;
}

// What the fixture's presentation hook makes of the report's last point (the
// edits of tests/test_report_sink_equity.cpp's "present" mode).
pf_equity_point_t presented(pf_equity_point_t point) {
    point.time_ms += step_ms;
    point.equity -= 0.75;
    point.open_profit = -0.0;
    return point;
}

// Alternates one buy and one sell every 11th calculation (a long position open
// about half the tape, many closed rows), or stays flat. Prices are exact binary
// fractions. The members it publishes are protected engine state.
class CurveProbe final : public NativeStrategyHost {
public:
    bool probe_trade = true;
    bool probe_present = false;
    std::int64_t probe_calculations = 0;
    std::size_t probe_peak_size = 0;
    std::size_t probe_peak_capacity = 0;
    mutable std::size_t probe_presentations = 0;

    // Run facts a bounded curve must not disturb: the four running extremes, the
    // buy&hold basis and the bars-in-market counter. Compared bit for bit.
    struct Facts {
        double max_equity, min_equity, max_drawdown, max_runup, first_bar_open;
        std::int64_t bars_in_market;
    };

    void on_native_run_begin() override {
        probe_calculations = 0;
        probe_peak_size = 0;
        probe_peak_capacity = 0;
        probe_presentations = 0;
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const std::int64_t k = probe_calculations++;
        probe_peak_size = std::max(probe_peak_size, equity_curve_.size());
        probe_peak_capacity = std::max(probe_peak_capacity, equity_curve_.capacity());
        if (probe_trade && k % 11 == 0) {
            const bool buy = (k / 11) % 2 == 0;
            native_order::Request order;
            order.intent = native_order::Transact{buy ? 1.0 : -1.0};
            order.label = buy ? "open" : "close";
            submit(order);
        }
    }
    // DESIGN 3.2: presentation edits only the report's own copy of the last
    // point, never the engine's held point or its scratch prefix.
    void present_report(ReportC* report) const override {
        ++probe_presentations;
        if (probe_present && report->equity_curve_len > 0) {
            pf_equity_point_t& last = report->equity_curve[report->equity_curve_len - 1];
            last = presented(last);
        }
    }

    const std::vector<pf_equity_point_t>& probe_curve() const { return equity_curve_; }
    std::size_t probe_size() const { return equity_curve_.size(); }
    std::size_t probe_capacity() const { return equity_curve_.capacity(); }
    // The held point (last_equity_point), by value; false when none is held.
    bool probe_held(pf_equity_point_t* out) const {
        const pf_equity_point_t* point = last_equity_point();
        if (point == nullptr) return false;
        *out = *point;
        return true;
    }
    Facts probe_facts() const {
        return {max_equity_, min_equity_, max_drawdown_, max_runup_, first_bar_open_,
                bars_in_market_};
    }
    // The engine's scratch curve: its execution prefix, without the held point
    // even after fill. Flushes pending writes and borrows the engine-owned
    // region, which lives until the next run or handle destruction.
    // {-1, 0, 0} without an ordinary scratch owner.
    pineforge::metrics::ScratchCurveRegion probe_scratch() const {
        return report_sink_equity_scratch_region();
    }
};

static_assert(sizeof(CurveProbe::Facts) == 5 * sizeof(double) + sizeof(std::int64_t),
              "Facts is compared with memcmp and must have no padding");

bool same_facts(const CurveProbe::Facts& actual, const CurveProbe::Facts& expected) {
    return std::memcmp(&actual, &expected, sizeof actual) == 0;
}

std::vector<Bar> tape(std::int64_t bars) {
    std::vector<Bar> feed;
    feed.reserve(static_cast<std::size_t>(bars));
    std::uint32_t state = 0x6b4f1u;
    for (std::int64_t i = 0; i < bars; ++i) {
        state = state * 1664525u + 1013904223u;
        const double price = 100.0 + (static_cast<int>(state % 801u) - 400) * 0.125;
        feed.push_back({price, price + 0.5, price - 0.5, price + 0.125, 1.0,
                        start_ms + i * step_ms});
    }
    return feed;
}

NativeRunSpec spec(std::uint64_t run_number) {
    NativeRunSpec result;
    result.identity = {"l6b4-fix1", run_number};
    result.input_tf = result.script_tf = "1";
    result.tickerid = "TEST:L6B4F1";
    result.timezone = "UTC";
    result.session = "24x7";
    result.initial_capital = initial_capital;
    result.point_value = result.account_fx = 1.0;
    result.price_tick = 0.01;
    result.report_policy = NativeReportPolicy::KernelRecorded;
    result.report_open_position_at_end = true;
    return result;
}

void configure(CurveProbe& host, std::uint64_t run_number) {
    require(host.configure_native(spec(run_number)).status == NativeSetupStatus::Applied,
            "native configure refused");
}

int observed(void* context, const pf_execution_boundary_v1* boundary,
             pf_boundary_receipt_v1* out) {
    ++*static_cast<int*>(context);
    *out = {sizeof(*out), 1, boundary->run_generation, 0, 0, 0, 0};
    return 0;
}

void observe(CurveProbe& host, int& calls) {
    const pf_execution_observer_v1 observer{sizeof observer, 1, &calls, observed};
    require(pineforge::native_set_execution_observer_v1(&host, &observer) == 0,
            "observer install refused");
}

void install(BacktestEngine& host, const std::string& path, const char* scratch_dir) {
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "l6b4-fix1", scratch_dir};
    require(host.set_report_sink_v1(&sink) == 0, "sink install refused");
}

void run_tape(CurveProbe& host, const std::vector<Bar>& feed) {
    host.run(feed.data(), static_cast<int>(feed.size()));
    require(host.last_error().empty() && host.last_run_status() == 0,
            "fixture run failed: " + host.last_error());
    require(host.probe_calculations == static_cast<std::int64_t>(feed.size()),
            "fixture did not calculate every script bar");
}

pf_report_sink_receipt_v1 receipt(const BacktestEngine& host) {
    pf_report_sink_receipt_v1 result{};
    result.struct_size = sizeof result;
    result.version = 1;
    require(host.report_sink_receipt_v1(&result) == 0, "receipt query refused");
    return result;
}

std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string equity_record(const pf_equity_point_t& point) {
    return "[\"E\"," + std::to_string(point.time_ms) + ',' + rs::f64(point.equity) + ','
        + rs::f64(point.open_profit) + "]\n";
}

std::string equity_lines(const std::string& bytes) {
    std::string result;
    for (std::size_t at = 0; at < bytes.size();) {
        const std::size_t end = bytes.find('\n', at);
        require(end != std::string::npos, "sink lacks final LF");
        if (bytes.compare(at, 5, "[\"E\",") == 0) result.append(bytes, at, end + 1 - at);
        at = end + 1;
    }
    return result;
}

// Owning report view: fill_report allocates, free_report releases.
struct Filled {
    ReportC report{};
    explicit Filled(const BacktestEngine& engine) { engine.fill_report(&report); }
    ~Filled() { BacktestEngine::free_report(&report); }
    Filled(const Filled&) = delete;
    Filled& operator=(const Filled&) = delete;
};

// Anonymous (unlinked) regular files this process holds: scratch files are.
int anonymous_files() {
    int count = 0;
    for (int descriptor = 0; descriptor < 1024; ++descriptor) {
        struct stat status{};
        if (::fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode)
            && status.st_nlink == 0) ++count;
    }
    return count;
}

bool directory_empty(const std::string& path) {
    DIR* directory = ::opendir(path.c_str());
    require(directory != nullptr, "cannot open directory " + path);
    bool result = true;
    while (const dirent* entry = ::readdir(directory)) {
        if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0)
            result = false;
    }
    ::closedir(directory);
    return result;
}

void remove_tree(const std::string& path) {
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0) return;
    if (!S_ISDIR(status.st_mode)) {
        ::unlink(path.c_str());
        return;
    }
    ::chmod(path.c_str(), 0700);
    std::vector<std::string> names;
    if (DIR* directory = ::opendir(path.c_str())) {
        while (const dirent* entry = ::readdir(directory)) {
            const std::string name = entry->d_name;
            if (name != "." && name != "..") names.push_back(name);
        }
        ::closedir(directory);
    }
    for (const std::string& name : names) remove_tree(path + "/" + name);
    ::rmdir(path.c_str());
}

// One directory for sink output, one (separate, initially empty) for scratch.
struct TestRoot {
    std::string path = "/tmp/pineforge-l6b4-fix1-XXXXXX";
    TestRoot() {
        require(::mkdtemp(path.data()) != nullptr, "cannot create test directory");
        require(::mkdir(out().c_str(), 0700) == 0, "cannot create sink directory");
        require(::mkdir(scratch().c_str(), 0700) == 0, "cannot create scratch directory");
    }
    std::string out() const { return path + "/out"; }
    std::string scratch() const { return path + "/scratch"; }
    void remove_all() const { remove_tree(path); }
};

// (c) The sink-off contract, from index formulas and the host's own report copy.
void check_sink_off_curve(const CurveProbe& host, const Filled& filled,
                          const std::vector<Bar>& feed, bool trade) {
    const auto bars = static_cast<std::int64_t>(feed.size());
    const std::vector<pf_equity_point_t>& curve = host.probe_curve();
    require(host.last_error().empty() && host.last_run_status() == 0,
            "sink-off run failed: " + host.last_error());
    require(host.probe_calculations == bars, "sink-off run skipped script bars");
    require(static_cast<std::int64_t>(curve.size()) == bars,
            "sink-off curve is not one point per script bar");
    for (std::int64_t i = 0; i < bars; ++i) {
        const pf_equity_point_t& point = curve[static_cast<std::size_t>(i)];
        if (point.time_ms != feed[static_cast<std::size_t>(i)].timestamp)
            fail_test("sink-off point " + std::to_string(i)
                      + " is not labelled with its script bar's open");
        // A flat strategy never leaves the initial capital: no engine value involved.
        if (!trade && (point.equity != initial_capital || point.open_profit != 0.0))
            fail_test("flat sink-off point " + std::to_string(i) + " moved off the capital");
    }
    const ReportC& report = filled.report;
    require(report.equity_curve_len == bars && report.script_bars_processed == bars
                && report.equity_curve != nullptr,
            "sink-off report does not publish the whole curve");
    // The report publishes the engine's curve, but for the presentation hook's
    // edit of its own copy of the last point (the loop above proved the engine's
    // point itself is still labelled with its bar's open).
    const pf_equity_point_t report_last = host.probe_present ? presented(curve.back()) : curve.back();
    require(std::memcmp(report.equity_curve, curve.data(),
                        (curve.size() - 1) * sizeof(pf_equity_point_t)) == 0
                && same_point(report.equity_curve[curve.size() - 1], report_last),
            "sink-off report curve differs from the engine's curve");
    const pf_report_sink_receipt_v1 none = receipt(host);
    require(none.count_e == 0 && none.complete == 0, "sink-off receipt is not empty");
    const pineforge::metrics::ScratchCurveRegion scratch = host.probe_scratch();
    require(scratch.descriptor == -1 && scratch.point_count == 0,
            "a sink-off run owns a scratch curve");
}

// The reference: a sink-off run of the tape, kept alive for its in-memory curve.
struct Oracle {
    std::vector<Bar> feed;
    int observer_calls = 0;
    CurveProbe host;
    std::unique_ptr<Filled> report;

    Oracle(std::int64_t bars, bool trade, bool present, bool observer = false) : feed(tape(bars)) {
        host.probe_trade = trade;
        host.probe_present = present;
        const int anonymous_before = anonymous_files();
        configure(host, 1);
        if (observer) observe(host, observer_calls);
        run_tape(host, feed);
        report = std::make_unique<Filled>(host);
        require(anonymous_files() == anonymous_before,
                "sink-off run opened an anonymous scratch file");
        check_sink_off_curve(host, *report, feed, trade);
    }
    const std::vector<pf_equity_point_t>& curve() const { return host.probe_curve(); }
};

// What the fixture can observe of equity_curve_ over one run on a fresh host.
struct Peaks {
    std::size_t size = 0;
    std::size_t capacity = 0;
    std::size_t final_size = 0;
};

Peaks measure(const TestRoot& root, const std::string& name, std::int64_t bars, bool sink_on) {
    const std::vector<Bar> feed = tape(bars);
    CurveProbe host;
    configure(host, 1);
    if (sink_on) install(host, root.out() + "/" + name + ".ndjson", root.scratch().c_str());
    run_tape(host, feed);
    Peaks peaks;
    peaks.size = host.probe_peak_size;
    peaks.capacity = host.probe_peak_capacity;
    const auto sample = [&] {
        peaks.size = std::max(peaks.size, host.probe_size());
        peaks.capacity = std::max(peaks.capacity, host.probe_capacity());
    };
    sample();  // after the run
    {
        const Filled first(host);
        sample();  // after a fill
        const Filled second(host);
        sample();  // after a second fill
    }
    peaks.final_size = host.probe_size();
    return peaks;
}

// (a) equity_curve_ is bounded with a sink on.
void bounded_curve(const TestRoot& root) {
    scenario = "(a) bounded equity_curve_";
    const std::int64_t short_bars = batch + 41;
    const std::int64_t long_bars = 4 * short_bars;
    const Peaks off_short = measure(root, "bounded-off-short", short_bars, false);
    const Peaks off_long = measure(root, "bounded-off-long", long_bars, false);
    const Peaks on_short = measure(root, "bounded-on-short", short_bars, true);
    const Peaks on_long = measure(root, "bounded-on-long", long_bars, true);
    // Positive control: the sink-off curve is the whole history and the probes see it grow.
    require(off_short.final_size == static_cast<std::size_t>(short_bars)
                && off_long.final_size == static_cast<std::size_t>(long_bars),
            "sink-off curve is not one point per script bar");
    require(off_long.size > off_short.size,
            "the probe does not see the sink-off curve grow with the script bars");
    if (on_long.size != on_short.size || on_long.capacity != on_short.capacity)
        fail_test("equity_curve_ grows with the script bars with a sink on: peak size "
                  + std::to_string(on_short.size) + " (" + std::to_string(short_bars)
                  + " bars) vs " + std::to_string(on_long.size) + " ("
                  + std::to_string(long_bars) + " bars), peak capacity "
                  + std::to_string(on_short.capacity) + " vs " + std::to_string(on_long.capacity));
    // Not merely equal: smaller than even the shorter tape's script bars.
    require(on_long.size < static_cast<std::size_t>(short_bars)
                && on_long.capacity < static_cast<std::size_t>(short_bars),
            "equity_curve_ keeps a number of points comparable to the script bars");
}

// Every point of a scratch region must be the oracle's point at that index.
// Returns the region's point count.
std::int64_t check_scratch_prefix(const pineforge::metrics::ScratchCurveRegion& region,
                                  const std::vector<pf_equity_point_t>& expected,
                                  const char* when) {
    const rs::ScratchCurveWindow window(region);
    const std::int64_t count = window.size();
    if (count > static_cast<std::int64_t>(expected.size()))
        fail_test(std::string("scratch curve holds more points than the run recorded (")
                  + when + ")");
    for (std::int64_t i = 0; i < count; ++i) {
        if (!same_point(window[i], expected[static_cast<std::size_t>(i)]))
            fail_test("scratch point " + std::to_string(i)
                      + " differs from the sink-off curve (" + when + ")");
    }
    return count;
}

// DESIGN 3.8: private, anonymous, close-on-exec.
void check_private_file(const pineforge::metrics::ScratchCurveRegion& region) {
    const int descriptor = scratch_fixture::descriptor(region);
    struct stat status{};
    require(::fstat(descriptor, &status) == 0, "scratch descriptor is not open");
    require(S_ISREG(status.st_mode) && status.st_nlink == 0 && (status.st_mode & 0777) == 0600,
            "scratch curve is not a private unlinked regular file");
    const int flags = ::fcntl(descriptor, F_GETFD);
    require(flags != -1 && (flags & FD_CLOEXEC) != 0, "scratch curve does not close on exec");
}

// (b)+(c) One sink-on run of `on` (sink installed by the caller) over the
// oracle's tape, checked before fill, after fill and after a second fill.
// Observing the scratch curve flushes the engine's writer, so with
// `early_probe` false the first observation is after the fill: the engine's
// own flushes (a full batch mid-run, the staged tail at fill) are then the
// only ones the content checks depend on.
void sink_on_run(CurveProbe& on, const Oracle& oracle, std::uint64_t run_number,
                 const std::string& path, const std::string& scratch_dir, bool early_probe) {
    const std::vector<pf_equity_point_t>& expected = oracle.curve();
    const auto bars = static_cast<std::int64_t>(expected.size());
    configure(on, run_number);
    run_tape(on, oracle.feed);

    // Before fill: the E hold-back keeps one point; every other one was released
    // to the E records AND to the scratch curve, which excludes the held point.
    const pf_report_sink_receipt_v1 pending = receipt(on);
    require(pending.complete == 0 && pending.count_e == bars - 1,
            "E hold-back: a " + std::to_string(bars) + "-point run must emit "
            + std::to_string(bars - 1) + " E records before fill, not "
            + std::to_string(pending.count_e));
    pf_equity_point_t held{};
    require(on.probe_held(&held), "no held point before fill");
    require(same_point(held, expected.back()),
            "the held point differs from the sink-off curve's last point");
    if (early_probe) {
        const pineforge::metrics::ScratchCurveRegion before = on.probe_scratch();
        const std::int64_t committed = check_scratch_prefix(before, expected, "before fill");
        require(committed == pending.count_e,
                "scratch curve holds " + std::to_string(committed)
                + " points but the E records carry " + std::to_string(pending.count_e)
                + " (before fill)");
        require(committed + 1 == bars,
                "scratch curve plus the held point is not the whole history: "
                + std::to_string(committed) + " + 1 of " + std::to_string(bars) + " points");
        if (committed > 0) check_private_file(before);
    }
    require(directory_empty(scratch_dir),
            "the scratch curve has a directory entry (opened, then unlinked)");
    require(same_facts(on.probe_facts(), oracle.host.probe_facts()),
            "run facts (extremes, buy&hold basis, bars in market) differ from the sink-off run "
            "(before fill)");

    // After fill: the held point is presented through a copy and written as the
    // last E record; the engine's own held point and the scratch prefix stay
    // the execution values, so the E records are exactly scratch + held.
    const Filled filled(on);
    const ReportC& report = filled.report;
    require(report.equity_curve == nullptr && report.equity_curve_len == 0,
            "sink report must expose summary only");
    const pf_report_sink_receipt_v1 done = receipt(on);
    require(done.complete == 1 && done.count_e == bars, "completed receipt has the wrong E count");
    const std::int64_t all = check_scratch_prefix(on.probe_scratch(), expected, "after fill");
    pf_equity_point_t held_after{};
    require(on.probe_held(&held_after) && same_point(held_after, expected.back()),
            "the held point changed or vanished at fill");
    require(all + 1 == done.count_e,
            "scratch curve holds " + std::to_string(all) + " points; with the held point that is not the "
            + std::to_string(done.count_e) + " the E records carry (after fill)");
    // The E records are the sink-off REPORT's curve: the execution points, then
    // the presented copy of the last one (a presentation edit is part of it).
    const ReportC& want = oracle.report->report;
    std::string points;
    for (std::int64_t i = 0; i < want.equity_curve_len; ++i)
        points += equity_record(want.equity_curve[i]);
    const std::string bytes = read_file(path);
    require(equity_lines(bytes) == points, "E records differ from the sink-off report's curve");
    require(std::memcmp(&want.metrics, &report.metrics, sizeof want.metrics) == 0,
            "sink-on/off metrics differ");
    require(want.net_profit == report.net_profit && want.total_trades == report.total_trades
                && want.script_bars_processed == report.script_bars_processed,
            "sink-on/off report scalars differ");
    require(on.broker_state_hash() == oracle.host.broker_state_hash(), "sink changed broker hash");
    require(same_facts(on.probe_facts(), oracle.host.probe_facts()),
            "run facts differ from the sink-off run (after fill)");

    // A second fill reads the curve again and appends nothing.
    {
        const Filled again(on);
        require(std::memcmp(&report.metrics, &again.report.metrics, sizeof report.metrics) == 0,
                "repeated fill changed metrics");
    }
    require(read_file(path) == bytes, "repeated fill duplicated sink records");
    require(check_scratch_prefix(on.probe_scratch(), expected, "after a second fill") == all,
            "repeated fill changed the scratch curve");
}

struct RemovalPeaks {
    Peaks engine;
    std::size_t report_peak = 0;
    std::size_t report_capacity = 0;
    std::size_t off_report_peak = 0;
    std::size_t off_report_capacity = 0;
};

// (e) Count the owned report/capture arrays, not only the public lengths:
// finish_report_sink frees its temporary array before the caller sees it.
// Counts outlive their hosts, including the observer's retained capture.
RemovalPeaks measure_removal(const TestRoot& root, const std::string& name,
                             std::int64_t bars, bool observer) {
    scenario = "(e) " + name;
    report_arrays::Counts off_arrays;
    std::unique_ptr<Oracle> oracle;
    {
        const report_arrays::Scope count(off_arrays);
        oracle = std::make_unique<Oracle>(bars, false, true, observer);
    }
    report_arrays::Counts arrays;
    int observer_calls = 0;
    CurveProbe on;
    on.probe_trade = false;  // no other report arrays enter the allocation census
    on.probe_present = true;
    configure(on, 1);
    if (observer) observe(on, observer_calls);
    const std::string path = root.out() + "/" + name + ".ndjson";
    install(on, path, root.scratch().c_str());
    {
        const report_arrays::Scope count(arrays);
        run_tape(on, oracle->feed);
    }
    const std::size_t held_size = on.probe_size();
    const std::size_t held_capacity = on.probe_capacity();
    const std::size_t held_bytes = held_size * sizeof(pf_equity_point_t);
    require(held_size > 0 && held_size < oracle->curve().size(),
            "removal fixture has no bounded held tail");
    RemovalPeaks peaks;
    peaks.engine.size = std::max(on.probe_peak_size, held_size);
    peaks.engine.capacity = std::max(on.probe_peak_capacity, held_capacity);
    const auto sample = [&] {
        peaks.engine.size = std::max(peaks.engine.size, on.probe_size());
        peaks.engine.capacity = std::max(peaks.engine.capacity, on.probe_capacity());
        require(peaks.engine.size == held_size && peaks.engine.capacity == held_capacity,
                "sink removal or report fill rehydrated equity_curve_");
        require(arrays.peak_bytes <= held_bytes && arrays.capacity_bytes <= held_bytes,
                "sink removal or report fill allocated more than the counted report tail");
    };
    const auto refuse_removal = [&] {
        const auto before = receipt(on);
        const auto before_scratch = on.probe_scratch();
        const std::size_t allocations_before = arrays.allocations;
        int status;
        {
            const report_arrays::Scope count(arrays);
            status = on.set_report_sink_v1(nullptr);
        }
        sample();  // memory assertions precede the typed refusal assertion
        require(arrays.allocations == allocations_before,
                "sink removal allocated another curve array");
        require(status == -3, "live ordinary scratch removal did not return typed setter refusal -3");
        const auto after = receipt(on);
        require(std::memcmp(&before, &after, sizeof before) == 0,
                "refused removal changed the installed sink's receipt");
        const auto after_scratch = on.probe_scratch();
        require(after_scratch.scratch == before_scratch.scratch
                    && after_scratch.descriptor == before_scratch.descriptor
                    && after_scratch.point_count == before_scratch.point_count,
                "refused removal dropped or changed the scratch owner");
        check_private_file(after_scratch);
        require(on.last_run_status() == 0 && on.last_error().empty(),
                "ordinary setter refusal rewrote the successful run outcome");
    };
    const auto check_report = [&](const ReportC& report) {
        const ReportC& want = oracle->report->report;
        require(report.equity_curve == nullptr && report.equity_curve_len == 0,
                "refused removal stopped returning a summary report");
        require(std::memcmp(&report.metrics, &want.metrics, sizeof want.metrics) == 0
                    && report.net_profit == want.net_profit
                    && report.total_trades == want.total_trades
                    && report.script_bars_processed == want.script_bars_processed,
                "report after refused removal differs from the sink-off oracle");
    };

    // Before the first fill too: the ordinary tail is not staged yet, whereas
    // an observer-only run already owns its captured one-point presentation.
    refuse_removal();
    {
        const report_arrays::Scope count(arrays);
        const Filled first(on);
        check_report(first.report);
        sample();
    }
    require(arrays.peak_bytes == held_bytes && arrays.capacity_bytes == held_bytes,
            "the census did not observe the report's counted held-tail allocation");
    const std::string bytes = read_file(path);
    const std::size_t presentations_before = on.probe_presentations;
    const int callbacks_before = observer_calls;
    const auto completed = receipt(on);
    require(completed.complete != 0
                && completed.count_e == static_cast<std::int64_t>(oracle->curve().size()),
            "removal fixture never completed its ordinary sink");

    // The completed generation remains readable after another refused detach.
    refuse_removal();
    {
        const report_arrays::Scope count(arrays);
        const Filled again(on);
        check_report(again.report);
        sample();
    }
    {
        const report_arrays::Scope count(arrays);
        const Filled again(on);
        check_report(again.report);
        sample();
    }
    require(arrays.peak_bytes == held_bytes && arrays.capacity_bytes == held_bytes,
            "report-array peak or capacity changed after refused removal");
    require(arrays.live_bytes == (observer ? held_bytes : 0),
            "report fill retained an extra curve array");
    if (observer) {
        require(observer_calls > 0 && observer_calls == oracle->observer_calls
                    && observer_calls == callbacks_before
                    && on.probe_presentations == presentations_before,
                "observer-only run did not reuse its sealed capture");
    }
    std::string points;
    const ReportC& want = oracle->report->report;
    for (std::int64_t i = 0; i < want.equity_curve_len; ++i)
        points += equity_record(want.equity_curve[i]);
    require(equity_lines(bytes) == points && read_file(path) == bytes,
            "refused removal or repeated reads changed the oracle's E records");
    require(check_scratch_prefix(on.probe_scratch(), oracle->curve(), "after refused removal")
                + static_cast<std::int64_t>(held_size) == completed.count_e,
            "scratch no longer holds the oracle's execution prefix");
    require(on.broker_state_hash() == oracle->host.broker_state_hash()
                && same_facts(on.probe_facts(), oracle->host.probe_facts()),
            "refused removal changed the broker hash or run facts");
    peaks.engine.final_size = on.probe_size();
    peaks.report_peak = arrays.peak_bytes / sizeof(pf_equity_point_t);
    peaks.report_capacity = arrays.capacity_bytes / sizeof(pf_equity_point_t);
    peaks.off_report_peak = off_arrays.peak_bytes / sizeof(pf_equity_point_t);
    peaks.off_report_capacity = off_arrays.capacity_bytes / sizeof(pf_equity_point_t);
    require(peaks.off_report_capacity == oracle->curve().size(),
            "array census does not see the sink-off report's full curve");
    return peaks;
}

void bounded_removal(const TestRoot& root) {
    const std::int64_t short_bars = batch + 41;
    const std::int64_t long_bars = 4 * short_bars;
    const bool modes[] = {false, true};
    for (bool observer : modes) {
        const std::string name = observer ? "removal-observer" : "removal-ordinary";
        const RemovalPeaks small = measure_removal(root, name + "-short", short_bars, observer);
        const RemovalPeaks large = measure_removal(root, name + "-long", long_bars, observer);
        require(large.engine.size == small.engine.size
                    && large.engine.capacity == small.engine.capacity
                    && large.report_peak == small.report_peak
                    && large.report_capacity == small.report_capacity,
                "engine or report-array peak/capacity after removal grows on the 4x tape");
        require(large.engine.size < static_cast<std::size_t>(short_bars)
                    && large.engine.capacity < static_cast<std::size_t>(short_bars)
                    && large.report_peak < static_cast<std::size_t>(short_bars)
                    && large.report_capacity < static_cast<std::size_t>(short_bars),
                "removal keeps a curve comparable to the shorter tape's bar count");
        require(large.off_report_peak > small.off_report_peak
                    && large.off_report_capacity > small.off_report_capacity,
                "array census does not see the sink-off positive control grow");
    }
}

struct Plan {
    std::string name;
    std::int64_t bars;
    bool trade;
    bool present;      // the host's presentation hook edits the report's last point
    bool early_probe;  // observe the scratch curve before the fill
};

// (b)+(c) A fresh sink-on host over a tape of plan.bars script bars; the scratch
// curve dies with its handle.
void prove_curve(const TestRoot& root, const Plan& plan) {
    scenario = plan.name;
    const Oracle oracle(plan.bars, plan.trade, plan.present);
    const std::string path = root.out() + "/" + plan.name + ".ndjson";
    auto on = std::make_unique<CurveProbe>();
    on->probe_trade = plan.trade;
    on->probe_present = plan.present;
    install(*on, path, root.scratch().c_str());
    sink_on_run(*on, oracle, 1, path, root.scratch(), plan.early_probe);
    const int descriptor = scratch_fixture::descriptor(on->probe_scratch());
    require(descriptor >= 0, "no scratch descriptor after a completed sink-on run");
    on.reset();
    errno = 0;
    require(::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF,
            "the scratch curve outlives its handle");
}

// (b)+(c) Replacing a sink preserves metrics; removal after a scratch-backed
// run refuses. A handle with its sink removed before running stays sink-off.
void reuse_cycle(const TestRoot& root) {
    scenario = "reuse: oracles";
    const Oracle big(batch + 41, true, false);
    const Oracle small(257, true, false);
    const std::string first = root.out() + "/reuse-first.ndjson";
    const std::string second = root.out() + "/reuse-second.ndjson";
    CurveProbe on;
    install(on, first, root.scratch().c_str());
    scenario = "reuse: sink on, long tape";
    sink_on_run(on, big, 1, first, root.scratch(), true);
    // Replacing the sink while the completed run's scratch curve is live must
    // not take away the metric source of that run's report.
    scenario = "reuse: sink replaced after a completed run";
    const std::string first_bytes = read_file(first);
    install(on, second, root.scratch().c_str());
    {
        const Filled replaced(on);
        require(std::memcmp(&replaced.report.metrics, &big.report->report.metrics,
                            sizeof replaced.report.metrics) == 0,
                "metrics of the completed run changed when the sink was replaced");
    }
    require(read_file(first) == first_bytes, "replacing the sink rewrote the old sink's file");
    // The shorter tape must start a new scratch curve, not extend the old one.
    scenario = "reuse: sink on, shorter tape on the same handle";
    sink_on_run(on, small, 2, second, root.scratch(), true);
    // A completed scratch-backed generation cannot be converted to an array.
    scenario = "reuse: sink removal refused after a completed run";
    require(on.set_report_sink_v1(nullptr) == -3, "live scratch removal was not refused");
    {
        const Filled retained(on);
        require(retained.report.equity_curve == nullptr && retained.report.equity_curve_len == 0,
                "refused removal exposed a full curve array");
        require(std::memcmp(&retained.report.metrics, &small.report->report.metrics,
                            sizeof retained.report.metrics) == 0,
                "the report after refused removal differs from the sink-off oracle's");
    }
    // Removal is still accepted without a scratch owner. Such a handle can
    // run sink-off, then opt back in and drop its full-curve capacity.
    CurveProbe off;
    const std::string off_path = root.out() + "/reuse-off.ndjson";
    install(off, off_path, root.scratch().c_str());
    require(off.set_report_sink_v1(nullptr) == 0, "sink removal before a run refused");
    scenario = "reuse: sink-off run on the same handle";
    configure(off, 3);
    run_tape(off, big.feed);
    {
        const Filled filled(off);
        check_sink_off_curve(off, filled, big.feed, true);
        require(off.probe_curve().size() == big.curve().size()
                    && std::memcmp(off.probe_curve().data(), big.curve().data(),
                                   big.curve().size() * sizeof(pf_equity_point_t)) == 0,
                "sink-off curve on a reused handle differs from the oracle");
        require(std::memcmp(&filled.report.metrics, &big.report->report.metrics,
                            sizeof filled.report.metrics) == 0,
                "sink-off metrics on a reused handle differ from the oracle");
    }
    // Back on, after a run that left capacity for the whole long curve behind:
    // the new sink-on run must not keep it (same as a fresh sink-on host).
    scenario = "reuse: sink back on after a sink-off run";
    install(off, off_path, root.scratch().c_str());
    sink_on_run(off, small, 4, off_path, root.scratch(), true);
    const Peaks fresh = measure(root, "reuse-fresh", 257, true);
    if (off.probe_peak_size > fresh.size || off.probe_peak_capacity > fresh.capacity)
        fail_test("a sink-on run on a handle that ran sink-off keeps the old curve's storage: peak size "
                  + std::to_string(off.probe_peak_size) + ", peak capacity "
                  + std::to_string(off.probe_peak_capacity) + " vs "
                  + std::to_string(fresh.size) + " and " + std::to_string(fresh.capacity)
                  + " on a fresh host");
}

// (d) A scratch_dir the engine cannot create its file in fails the run, typed.
void scratch_failure(const TestRoot& root, const std::string& name,
                     const std::string& scratch_dir) {
    scenario = "(d) " + name;
    const std::vector<Bar> feed = tape(64);
    CurveProbe host;
    configure(host, 1);
    const std::string path = root.out() + "/" + name + ".ndjson";
    install(host, path, scratch_dir.c_str());
    try {
        host.run(feed.data(), static_cast<int>(feed.size()));
    } catch (const std::exception& error) {
        // A native host records a run failure (strategy_get_last_error_code);
        // run() does not throw it.
        fail_test(std::string("the scratch failure escaped run() as an exception: ") + error.what());
    }
    require(host.last_run_status() != 0,
            "the sink-on run succeeded although scratch_dir cannot hold a file: the engine "
            "fell back to RAM or never wrote a scratch curve");
    const std::string recorded = pineforge::run_failure_code_of(host);
    require(recorded == "report_sink_failed",
            "scratch failure is not the typed report_sink_failed (code '" + recorded
            + "', text '" + host.last_error() + "')");
    require(std::string(pineforge::run_failure_args_of(host)) == "{\"reason\":\"scratch_io\"}",
            "scratch create failure keeps its reason through the run boundary without sizes");
    // Not the sink file's own I/O error: the text is the scratch file's (ReportScratchIoError).
    require(host.last_error().find("report scratch create") != std::string::npos,
            "the typed failure is not the scratch file's: '" + host.last_error() + "'");
    require(receipt(host).complete == 0, "a failed scratch run completed its sink");
    require(read_file(path).find("{\"trailer\":") == std::string::npos,
            "a failed scratch run wrote a trailer");
}

void scratch_failures(const TestRoot& root) {
    // Missing directory (ENOENT) and a regular file in its place (ENOTDIR).
    scratch_failure(root, "missing-directory", root.path + "/missing/scratch");
    const std::string regular = root.path + "/not-a-directory";
    {
        std::ofstream file(regular);
        require(bool(file), "cannot create the regular-file fixture");
    }
    scratch_failure(root, "not-a-directory", regular);
    // A directory this process cannot create files in (EACCES). The case needs
    // the precondition to hold: a privileged process bypasses directory modes.
    const std::string readonly = root.path + "/read-only";
    require(::mkdir(readonly.c_str(), 0500) == 0, "cannot create the read-only directory");
    const std::string probe = readonly + "/probe";
    const int opened = ::open(probe.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (opened >= 0) {
        ::close(opened);
        ::unlink(probe.c_str());
        std::puts("L6b-4 fix1: read-only scratch case skipped: this process bypasses directory modes");
        return;
    }
    scratch_failure(root, "read-only", readonly);
}

}  // namespace

int main() {
    std::string kept;
    try {
        const TestRoot root;
        kept = root.path;
        bounded_curve(root);
        bounded_removal(root);
        // Tapes at the edges of the hold-back (1-3 points) and of the scratch batch
        // (the first mid-run flush needs batch + 2 bars), a tape whose scratch is
        // first observed after the fill, a flat curve and a presentation edit.
        const Plan plans[] = {
            {"trade-1", 1, true, false, true},
            {"trade-2", 2, true, false, true},
            {"trade-3", 3, true, false, true},
            {"batch", batch, true, false, true},
            {"batch-plus-1", batch + 1, true, false, true},
            {"batch-plus-2", batch + 2, true, false, true},
            {"late-probe", batch + 41, true, false, false},
            {"flat", batch + 41, false, false, true},
            {"present-1", 1, true, true, true},
            {"present", 257, true, true, true},
        };
        for (const Plan& plan : plans) prove_curve(root, plan);
        reuse_cycle(root);
        scratch_failures(root);
        root.remove_all();
        std::puts("L6b-4 fix2: bounded equity_curve_ and removal report arrays, "
                  "scratch curve equal to the sink-off curve, "
                  "sink-off unchanged, typed removal and scratch failures: passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "L6b-4 fix2: %s (files kept in %s)\n", error.what(), kept.c_str());
        return 1;
    }
}
