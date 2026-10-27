// RED-FIRST CR-403 usage receipt coverage.
//
// The expected values below come from the callback, the independently read
// sink file and the existing arena query. None are copied from the receipt.
#include <pineforge/native_host.hpp>
#include <pineforge/pineforge.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

pineforge::NativeRunSpec usage_spec(const char* key) {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = key;
    spec.identity.run_number = 1;
    spec.input_tf = "1";
    spec.script_tf = "1";
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
    return spec;
}

class UsageHost final : public pineforge::NativeStrategyHost {
public:
    explicit UsageHost(bool fail) : fail_(fail) {}

private:
    void on_native_run_begin() override { bars_ = 0; }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++bars_;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (fail_ && bars_ == 2) throw std::runtime_error("receipt failure row");
    }

    bool fail_;
    int bars_ = 0;
};

std::vector<pineforge::Bar> bars(int count) {
    std::vector<pineforge::Bar> result;
    result.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const double close = 100.0 + i;
        result.push_back({close, close + 1.0, close - 1.0, close, 10.0,
                          INT64_C(1700000000000) + static_cast<std::int64_t>(i) * 60000});
    }
    return result;
}

struct Receipt {
    struct pf_run_usage_v1 value{};

    Receipt() {
        value.struct_size = sizeof value;
        value.version = 1;
        require(pf_run_usage_v1(&value) == 0, "usage receipt query failed");
    }
};

std::string temporary_path() {
    char pattern[] = "/tmp/pineforge-usage-XXXXXX";
    const int fd = ::mkstemp(pattern);
    require(fd >= 0, "mkstemp failed");
    ::close(fd);
    require(::unlink(pattern) == 0, "unlink failed");
    return pattern;
}

std::uint64_t file_bytes(const std::string& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    require(stream.good(), "sink file could not be read");
    return static_cast<std::uint64_t>(stream.tellg());
}

void query_is_transactional_before_first_run() {
    struct pf_run_usage_v1 value{};
    value.struct_size = 0;
    value.version = 1;
    value.cpu_user_ns = UINT64_C(0xdeadbeef);
    const struct pf_run_usage_v1 before = value;
    require(pf_run_usage_v1(&value) == -1, "pre-run usage query was accepted");
    require(std::memcmp(&value, &before, sizeof value) == 0,
            "failed usage query changed its output");
}

void successful_run_has_measured_usage() {
    constexpr int count = 2;
    UsageHost host(false);
    require(host.configure_native(usage_spec("usage-success")).status
                == pineforge::NativeSetupStatus::Applied,
            "usage success spec refused");
    const auto input = bars(count);
    host.run(input.data(), static_cast<int>(input.size()));
    require(host.native_state().kind == pineforge::NativeLifecycleKind::Completed,
            "usage success run failed");

    pf_report_t report{};
    host.fill_report(reinterpret_cast<pineforge::ReportC*>(&report));
    const Receipt receipt;
    require(receipt.value.bars_processed == static_cast<std::uint64_t>(count),
            "bars_processed did not equal the known script-bar count");
    require(receipt.value.cpu_user_ns + receipt.value.cpu_system_ns > 0,
            "CPU usage was not measured");
    require(receipt.value.run_wall_ns >= UINT64_C(40000000),
            "wall usage did not include the two 20 ms callback sleeps");
    require(receipt.value.peak_rss_bytes > 0, "RSS usage was not measured");
    const Receipt repeated;
    require(repeated.value.peak_rss_bytes >= receipt.value.peak_rss_bytes,
            "RSS high water was not monotone across reads");

    std::uint64_t high_water = 0;
    std::uint64_t capacity = 0;
    require(pf_report_scratch_arena_stats(&high_water, &capacity) == 0,
            "existing arena stats query failed");
    require(receipt.value.arena_high_water_bytes == high_water
                && receipt.value.arena_capacity_bytes == capacity,
            "usage arena values differ from the existing stats query");
    require((receipt.value.unmeasured_mask
             & PF_RUN_USAGE_UNMEASURED_REPORT_BYTES) != 0,
            "report-size unmeasured flag was omitted");
    pineforge::BacktestEngine::free_report(reinterpret_cast<pineforge::ReportC*>(&report));
}

void sink_usage_is_independently_counted() {
    const std::string path = temporary_path();
    UsageHost host(false);
    require(host.configure_native(usage_spec("usage-sink")).status
                == pineforge::NativeSetupStatus::Applied,
            "usage sink spec refused");
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "usage", "/tmp"};
    require(strategy_set_report_sink_v1(&host, &sink) == 0, "usage sink setter refused");
    const auto input = bars(2);
    host.run(input.data(), static_cast<int>(input.size()));
    require(host.native_state().kind == pineforge::NativeLifecycleKind::Completed,
            "usage sink run failed");
    pf_report_t report{};
    host.fill_report(reinterpret_cast<pineforge::ReportC*>(&report));
    const Receipt receipt;
    const std::uint64_t expected_rows =
        static_cast<std::uint64_t>(report.equity_curve_len + report.trades_len);
    require(receipt.value.sink_rows_written == expected_rows,
            "sink row count was not independently counted");
    require(receipt.value.sink_bytes_written == file_bytes(path),
            "sink byte count differs from the independently read file");
    require(receipt.value.scratch_file_count_peak > 0,
            "ScratchFile count peak was not measured");
    std::remove(path.c_str());
    pineforge::BacktestEngine::free_report(reinterpret_cast<pineforge::ReportC*>(&report));
}

void failed_run_keeps_receipt() {
    UsageHost host(true);
    require(host.configure_native(usage_spec("usage-failure")).status
                == pineforge::NativeSetupStatus::Applied,
            "usage failure spec refused");
    const auto input = bars(3);
    host.run(input.data(), static_cast<int>(input.size()));
    require(host.native_state().kind == pineforge::NativeLifecycleKind::Failed,
            "expected the callback failure");
    const Receipt receipt;
    require(receipt.value.bars_processed == 1,
            "failed receipt did not retain bars before the failure");
    require(receipt.value.run_wall_ns >= UINT64_C(20000000),
            "failed receipt did not retain wall time");
}

} // namespace

int main() {
    try {
        query_is_transactional_before_first_run();
        successful_run_has_measured_usage();
        sink_usage_is_independently_counted();
        failed_run_keeps_receipt();
        std::puts("test_run_usage: all rows passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_run_usage: %s\n", error.what());
        return 1;
    }
}
