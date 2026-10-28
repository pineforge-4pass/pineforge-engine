// Written before the cache-bound change. On b08fc6fd the real sink-run
// default is 128 rows: repeated 2,000-row scans fail the warm-pread bound.
// Linux interposition follows test_p2cx_tledger_io: count every pread in
// the measured accessor loops and forward it to the actual kernel syscall.
#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/report_sink.h>
#include <pineforge/selected_window.h>

#include "../src/native_execution_consumer.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace sink_scan_io {
bool measuring = false;
std::uint64_t reads = 0;

struct Measure {
    Measure() {
        if (measuring) throw std::logic_error("nested sink scan measurement");
        reads = 0;
        measuring = true;
    }
    ~Measure() { measuring = false; }
    std::uint64_t finish() {
        measuring = false;
        return reads;
    }
};
}

extern "C" ssize_t pread(int fd, void* data, std::size_t length, off_t offset) {
    if (sink_scan_io::measuring) ++sink_scan_io::reads;
    return static_cast<ssize_t>(::syscall(SYS_pread64, fd, data, length, offset));
}

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;

int checks = 0;
int failures = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failures; \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

constexpr int kTrades = 2000;
constexpr int kScans = 6;
constexpr std::int64_t kStart = 1704067200000LL;
constexpr std::int64_t kMinute = 60000;

bool same_bits(double a, double b) {
    return std::memcmp(&a, &b, sizeof a) == 0;
}

struct Directory {
    std::string path;
    Directory() {
        char pattern[] = "/tmp/pineforge-ledger-scan-XXXXXX";
        const char* created = mkdtemp(pattern);
        if (!created) throw std::runtime_error("sink scan mkdtemp failed");
        path = created;
    }
    ~Directory() {
        std::remove((path + "/small-cache.ndjson").c_str());
        std::remove((path + "/sink.ndjson").c_str());
        rmdir(path.c_str());
    }
};

struct Report {
    ReportC value{};
    ~Report() { BacktestEngine::free_report(&value); }
};

struct Value {
    double profit;
    std::string entry_id;
};

struct Host final : NativeStrategyHost {
    int calculations = 0;
    int scans = 0;
    std::uint64_t cold_reads = 0;
    std::uint64_t warm_reads = 0;
    bool force_small_cache = false;
    mutable std::uint64_t capture_hash = 0;
    mutable int presentations = 0;
    const std::vector<Value>* oracle = nullptr;
    std::vector<Value> values;

    std::size_t cache_rows() const { return trades_.cache_rows(); }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const int index = calculations++;
        if (index == 0 && force_small_cache) {
            // Override only the byte-oracle run, after run-begin selected its
            // budget and before any trade append or spill can freeze it.
            CHECK(trades_.empty());
            trades_.set_cache_rows(128);
        }
        if (index < 2 * kTrades) {
            const double side = (index / 2) % 2 == 0 ? 1.0 : -1.0;
            const double units = index % 2 == 0 ? side : -side;
            const auto id = std::string(index % 2 == 0 ? "entry-" : "exit-")
                + std::to_string(index / 2);
            const auto result = submit({no::Transact{units}, id, ""});
            CHECK(result.status == no::SubmitStatus::Accepted && result.handle.has_value());
        }
        if (closed_trade_count() != kTrades) return;
        // Every remaining callback scans the entire closed history. The
        // measured run uses no configuration setter or fabricated ledger rows.
        sink_scan_io::Measure measure;
        for (int i = 0; i < kTrades; ++i) {
            const double profit = closed_trade_profit(i);
            const auto id = closed_trade_entry_id(i);
            if (oracle) {
                const auto offset = static_cast<std::size_t>(scans * kTrades + i);
                CHECK(offset < oracle->size());
                if (offset < oracle->size()) {
                    CHECK(same_bits(profit, (*oracle)[offset].profit));
                    CHECK(id == (*oracle)[offset].entry_id);
                }
            } else {
                values.push_back({profit, id});
            }
        }
        const auto reads = measure.finish();
        if (scans == 0) cold_reads = reads;
        else warm_reads += reads;
        ++scans;
    }

    void present_report(ReportC*) const override {
        ++presentations;
        capture_hash = broker_state_hash();
    }
};

NativeRunSpec spec() {
    NativeRunSpec result;
    result.identity = {"ledger-sink-scan", 1};
    result.input_tf = result.script_tf = "1";
    result.ticker = "MOCK";
    result.tickerid = "TEST:MOCK";
    result.type = "crypto";
    result.currency = "USD";
    result.timezone = "UTC";
    result.session = "24x7";
    result.initial_capital = 10000.0;
    result.point_value = result.account_fx = 1.0;
    result.price_tick = 0.01;
    result.report_policy = NativeReportPolicy::KernelRecorded;
    return result;
}

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("sink scan report read failed");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

pf_report_sink_receipt_v1 receipt(const Host& host) {
    pf_report_sink_receipt_v1 result{};
    result.struct_size = sizeof result;
    result.version = 1;
    CHECK(host.report_sink_receipt_v1(&result) == 0);
    return result;
}

void forwarding_control(const Directory& directory) {
    detail::ScratchFile file;
    file.set_directory(directory.path);
    file.open("ledger-scan-counter");
    const char expected[] = "actual positional read";
    char actual[sizeof expected]{};
    file.write(0, expected, sizeof expected);
    sink_scan_io::Measure measure;
    file.read(0, actual, sizeof actual);
    const auto reads = measure.finish();
    CHECK(reads == 1);
    CHECK(std::memcmp(actual, expected, sizeof expected) == 0);
}

void sink_scan(const Directory& directory) {
    std::vector<Bar> bars;
    for (int i = 0; i < 2 * kTrades + kScans; ++i) {
        const double price = 100.0 + ((i / 2) % 7 - 3) * 0.001
            + (i % 2 == 0 ? 0.0 : 0.125);
        bars.push_back({price, price, price, price, 1.0, kStart + i * kMinute});
    }
    Host baseline, streamed;
    baseline.values.reserve(kTrades * kScans);
    const pf_selected_window_config_v1 window{
        sizeof window, 1, kStart, kStart + static_cast<std::int64_t>(bars.size()) * kMinute};
    for (auto* host : {&baseline, &streamed}) {
        CHECK(host->configure_native(spec()).status == NativeSetupStatus::Applied);
        CHECK(NativeExecutionConsumer::set_selected_window_v1(host, &window) == 0);
    }
    const auto sink_path = directory.path + "/sink.ndjson";
    const pf_report_sink_v1 sink{
        sizeof sink, 1, sink_path.c_str(), "ledger-sink-scan", directory.path.c_str()};
    CHECK(streamed.set_report_sink_v1(&sink) == 0);

    baseline.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(baseline.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(baseline.cache_rows() == TradeLedger::kDefaultCacheRows);
    CHECK(baseline.scans == kScans);
    CHECK(baseline.values.size() == kTrades * kScans);
    streamed.oracle = &baseline.values;
    streamed.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(streamed.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(streamed.calculations == static_cast<int>(bars.size()));
    CHECK(streamed.scans == kScans);
    CHECK(streamed.closed_trade_count() == kTrades);
    std::printf("sink scan: N=%d scans=%d first preads=%llu following preads=%llu "
                "(required < %d)\n", kTrades, streamed.scans,
                static_cast<unsigned long long>(streamed.cold_reads),
                static_cast<unsigned long long>(streamed.warm_reads), kTrades + 64);
    // The 128-row run must fail HERE, with the observed total above. All
    // later scans share this one budget, rather than a per-row/per-bar limit.
    CHECK(streamed.warm_reads < kTrades + 64);
    CHECK(streamed.cache_rows() == TradeLedger::kSinkCacheRows);
    CHECK(baseline.warm_reads == 0);
    CHECK(baseline.presentations == 1 && streamed.presentations == 1);
    CHECK(baseline.capture_hash == streamed.capture_hash);

    Report array, summary;
    baseline.fill_report(&array.value);
    streamed.fill_report(&summary.value);
    CHECK(array.value.trades_len == kTrades);
    CHECK(array.value.total_trades == summary.value.total_trades);
    CHECK(same_bits(array.value.net_profit, summary.value.net_profit));
    CHECK(std::memcmp(&array.value.metrics, &summary.value.metrics,
                      sizeof array.value.metrics) == 0);
    const auto sink_receipt = receipt(streamed);
    CHECK(sink_receipt.complete == 1 && sink_receipt.count_t == kTrades);
    const auto sink_bytes = read_file(sink_path);
    // Each T record starts with at least ["T",1 (six bytes); the rest of
    // every trade and all other records make the full file strictly larger.
    constexpr std::size_t minimum_bytes =
        static_cast<std::size_t>(kTrades) * (sizeof("[\"T\",1") - 1);
    CHECK(sink_bytes.size() > minimum_bytes);

    // The late sink in test_selected_metrics_l17b checks summary reads, not file publication. Produce
    // an independent file with a sink installed BEFORE this separate run.
    // On the fixed tree the measured sink uses the 65,536-row default;
    // forcing this oracle to 128 proves cache size cannot change wire bytes.
    Host small;
    small.force_small_cache = true;
    small.oracle = &baseline.values;
    CHECK(small.configure_native(spec()).status == NativeSetupStatus::Applied);
    CHECK(NativeExecutionConsumer::set_selected_window_v1(&small, &window) == 0);
    const auto small_path = directory.path + "/small-cache.ndjson";
    const pf_report_sink_v1 small_sink{
        sizeof small_sink, 1, small_path.c_str(), "ledger-sink-scan", directory.path.c_str()};
    CHECK(small.set_report_sink_v1(&small_sink) == 0);
    small.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(small.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(small.calculations == static_cast<int>(bars.size()));
    CHECK(small.scans == kScans);
    CHECK(small.closed_trade_count() == kTrades);
    CHECK(small.cache_rows() == 128);
    CHECK(small.presentations == 1 && small.capture_hash == streamed.capture_hash);
    Report small_summary;
    small.fill_report(&small_summary.value);
    CHECK(std::memcmp(&small_summary.value.metrics, &summary.value.metrics,
                      sizeof summary.value.metrics) == 0);
    const auto small_receipt = receipt(small);
    CHECK(small_receipt.complete == 1 && small_receipt.count_t == kTrades);
    CHECK(small_receipt.count_e == sink_receipt.count_e);
    CHECK(small_receipt.count_o == sink_receipt.count_o);
    CHECK(small_receipt.count_v == sink_receipt.count_v);
    CHECK(small_receipt.count_k == sink_receipt.count_k);
    CHECK(small_receipt.count_x == sink_receipt.count_x);
    CHECK(small_receipt.count_h == sink_receipt.count_h);
    CHECK(small_receipt.trades_retained == sink_receipt.trades_retained);
    CHECK(std::strcmp(small_receipt.sha256_hex, sink_receipt.sha256_hex) == 0);
    const auto small_bytes = read_file(small_path);
    CHECK(small_bytes.size() > minimum_bytes);
    std::printf("sink scan bytes: default=%zu small=%zu (each required > %zu)\n",
                sink_bytes.size(), small_bytes.size(), minimum_bytes);
    CHECK(small_bytes == sink_bytes);
}
}

int main() {
    try {
        Directory directory;
        forwarding_control(directory);
        sink_scan(directory);
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "sink scan exception: %s\n", error.what());
    }
    std::printf("test_ledger_sink_scan: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
