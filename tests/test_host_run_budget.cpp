#include <pineforge/engine.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/report_sink.h>
#include <pineforge/run_failure.hpp>
#include "../src/resource_clock.hpp"

#if __has_include(<pineforge/run_budget.h>)
#include <pineforge/run_budget.h>
#else
// Keep the baseline executable: an absent API fails a runtime assertion.
struct pf_run_budget_v1 {
    std::uint32_t struct_size, version;
    std::uint64_t wall_ms, cpu_ms;
};
#endif

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#define BUDGET_WEAK __attribute__((weak_import))
#else
#define BUDGET_WEAK __attribute__((weak))
#endif
extern "C" std::uint32_t pf_run_budget_version(void) BUDGET_WEAK;
extern "C" int strategy_set_run_budget_v1(pf_strategy_t, const pf_run_budget_v1*) BUDGET_WEAK;

namespace {
using namespace pineforge;
// The previous descriptor was 24 bytes. Extend only that header's storage so
// the new descriptor reaches its size rejection as a runtime red witness.
template<class T, class = void> struct BudgetStorage : T { std::uint64_t sink_stall_ms; };
template<class T> struct BudgetStorage<T, std::void_t<decltype(T::sink_stall_ms)>> : T {};
using Budget = BudgetStorage<pf_run_budget_v1>;
constexpr std::int64_t limit_ms = 100;
// Each row has at most one real spin of 2*limit_ms. Allow 100 such
// intervals plus one second for setup: ceil(100*2*100/1000)+1 = 21 s.
constexpr unsigned timeout_seconds = (100 * 2 * limit_ms + 999) / 1000 + 1;
std::int64_t wall = 0, cpu = 0;
bool expire_at_begin = false;
int wall_reads = 0;
int cpu_reads = 0;

void require(bool ok, const char* text) {
    if (!ok) throw std::runtime_error(text);
}

int read_clock(clockid_t kind, timespec* out) {
    if (kind == CLOCK_PROCESS_CPUTIME_ID) ++cpu_reads;
    if (kind == CLOCK_MONOTONIC) {
        ++wall_reads;
        if (expire_at_begin && wall_reads == 2) wall += 2 * limit_ms;
    }
    const auto value = kind == CLOCK_MONOTONIC ? wall : cpu;
    out->tv_sec = value / 1000;
    out->tv_nsec = (value % 1000) * 1000000;
    return 0;
}

struct ClockScope {
    ClockScope() {
        wall = cpu = wall_reads = 0;
        cpu_reads = 0;
        expire_at_begin = false;
        resource_clock::read_clock = read_clock;
    }
    ~ClockScope() { resource_clock::read_clock = nullptr; }
};

std::vector<Bar> tape(int count) {
    std::vector<Bar> bars;
    for (int i = 0; i != count; ++i)
        bars.push_back({100, 101, 99, 100, 1, INT64_C(1704067200000) + i * INT64_C(60000)});
    return bars;
}

struct Host final : NativeStrategyHost {
    int callbacks = 0, begins = 0, busy_result = 0;
    int spin_at = 1;
    int input_count = 3;
    bool spin = false, real_clock = false, spin_begin = false, at_limit = false;
    Host() {
        NativeRunSpec spec;
        spec.identity = {"budget-test", 1};
        spec.input_tf = spec.script_tf = "1";
        spec.tickerid = "TEST:BUDGET";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.initial_capital = 10000;
        spec.point_value = spec.account_fx = 1;
        spec.price_tick = 0.01;
        spec.report_policy = NativeReportPolicy::KernelRecorded;
        require(configure_native(spec).status == NativeSetupStatus::Applied, "configure budget host");
        set_broker_state_hash_recording(true);
    }
    pf_strategy_t handle() { return static_cast<BacktestEngine*>(this); }
    void burn() {
        // A real returning callback burns CPU, then advances the injected
        // clocks by exactly twice the budget. No scheduler sleep is used.
        timespec start{}, now{};
        require(::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start) == 0, "read spin clock");
        const auto elapsed = [&] {
            require(::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now) == 0, "read spin clock");
            // Subtract before converting: truncating a negative nanosecond
            // component separately could finish almost one millisecond early.
            return ((now.tv_sec - start.tv_sec) * INT64_C(1000000000)
                + now.tv_nsec - start.tv_nsec) / 1000000;
        };
        // Injected rows need one real millisecond; the real-clock row burns
        // 2*limit_ms, independently of any implementation clock read.
        const auto duration = real_clock ? 2 * limit_ms : limit_ms / 100;
        volatile std::uint64_t work = 1;
        while (elapsed() < duration) work = work * 1664525 + 1013904223;
        wall += 2 * limit_ms;
        cpu += 2 * limit_ms;
    }
    void on_native_run_begin() override {
        ++begins;
        if (spin_begin) burn();
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++callbacks;
        busy_result = strategy_set_run_budget_v1(handle(), nullptr);
        if (spin && spin_at > 1 && callbacks == spin_at)
            require(physical_position().lot_count > 0,
                    "sink budget fixture must fill its entry before the spin");
        if (callbacks == 1 || callbacks == 2) {
            const auto placed = submit({native_order::Transact{callbacks == 1 ? 1.0 : -1.0},
                                         callbacks == 1 ? "entry" : "exit", ""});
            require(placed.status == native_order::SubmitStatus::Accepted, "budget fixture trade");
        }
        if (spin && callbacks == spin_at) burn();
        if (at_limit && callbacks == 1) wall = cpu = limit_ms;
    }
    void execute() {
        const auto bars = tape(input_count);
        run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
            MagnifierDistribution::ENDPOINTS);
    }
};

void install(Host& host, std::uint64_t wall_limit, std::uint64_t cpu_limit,
             std::uint64_t sink_limit = 0) {
    require(strategy_set_run_budget_v1 != nullptr, "run budget setter is absent");
    Budget budget{};
    budget.struct_size = sizeof budget; budget.version = 1;
    budget.wall_ms = wall_limit; budget.cpu_ms = cpu_limit;
    budget.sink_stall_ms = sink_limit;
    require(strategy_set_run_budget_v1(host.handle(), &budget) == 0, "install run budget");
}

void expect_failure(Host& host, const char* kind, std::int64_t used) {
    require(host.last_run_status() == 1, "budget must terminate the run");
    require(std::string(strategy_get_last_error_code(host.handle())) == "run_budget_exceeded",
            "budget must retain its typed terminal code");
    const auto failure = run_failure_value_of(host);
    require(run_failure_code_class(failure.code) == RunFailureClass::resource
                && !run_failure_code_retryable(failure.code), "budget failure policy must be exact");
    const auto expected = std::string("{\"kind\":\"") + kind + "\",\"limit\":"
        + std::to_string(limit_ms) + ",\"used\":" + std::to_string(used) + "}";
    require(std::string(strategy_get_last_error_args(host.handle())) == expected,
            "budget kind, limit and used must be exact");
}

void spin_row(bool cpu_budget, bool in_begin) {
    ClockScope clock;
    Host host;
    host.spin = !in_begin;
    host.spin_begin = in_begin;
    install(host, cpu_budget ? 0 : limit_ms, cpu_budget ? limit_ms : 0);
    host.execute();
    expect_failure(host, cpu_budget ? "cpu" : "wall", 2 * limit_ms);
    require(host.callbacks == (in_begin ? 0 : 1),
            "budget must stop before the next input callback, not at teardown");
    if (!in_begin) require(host.busy_result == -3, "active budget replacement must refuse");
}

void begin_row() {
    ClockScope clock;
    Host host;
    install(host, limit_ms, 0);
    expire_at_begin = true;
    host.execute();
    expect_failure(host, "wall", 2 * limit_ms);
    require(host.begins == 0 && host.callbacks == 0, "begin budget must precede host callbacks");
}

std::string report_bytes(Host& host, bool success = true) {
    // Compare canonical wire bytes, including trades/equity/hash records
    // and the trailer digest. This does not compare object padding or heap
    // addresses, which are not output bytes.
    int fds[2];
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "create budget output socket");
    std::string bytes;
    bool read_ok = true;
    std::thread reader([&] {
        std::array<char, 4096> buffer{};
        for (;;) {
            const auto count = ::read(fds[0], buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) { read_ok = false; break; }
            if (!count) break;
            bytes.append(buffer.data(), static_cast<std::size_t>(count));
        }
        ::close(fds[0]);
    });
    ReportC report{};
    try {
        const pf_report_sink_fd_v1 sink{sizeof sink, 1, fds[1], "budget-output"};
        require(strategy_set_report_sink_fd_v1(host.handle(), &sink) == 0, "install budget output");
        host.execute();
        host.fill_report(&report);
        pf_report_sink_receipt_v1 receipt{sizeof receipt, 1};
        require(strategy_report_sink_receipt_v1(host.handle(), &receipt) == 0,
                "query budget output receipt");
        if (success) {
            require(host.last_run_status() == 0 && report.total_trades > 0,
                    "unchanged-budget fixture must finish real trades");
            require(receipt.complete,
                    "unchanged-budget fixture must publish a complete report");
        } else {
            require(host.last_run_status() == 1 && !receipt.complete && !receipt.sha256_hex[0],
                    "exceeded budget must not publish a successful receipt");
            pf_report_sink_summary_v1 summary{sizeof summary, 1};
            require(strategy_report_sink_summary_v1(host.handle(), &summary) == 0
                        && !summary.complete, "exceeded budget must not publish summary success");
            // Existing failed-run summaries may retain scalar diagnostics.
            // They must never expose completed sink arrays or a success latch.
            require(!report.trades && !report.equity_curve
                        && !report.trace && report.equity_curve_len == 0,
                    "exceeded budget must expose no completed sink arrays");
        }
    } catch (...) {
        BacktestEngine::free_report(&report);
        ::close(fds[1]);
        reader.join();
        throw;
    }
    BacktestEngine::free_report(&report);
    ::close(fds[1]);
    reader.join();
    require(read_ok, "read budget output");
    return bytes;
}

void sink_row() {
    ClockScope clock;
    Host host;
    host.spin = true;
    host.spin_at = 2; // Allow the first submitted entry to fill before failure.
    // A callback may seal the preceding input. Leave one input for that seal
    // and another boundary to stop on: N = spin_at + 2.
    host.input_count = host.spin_at + 2;
    install(host, limit_ms, 0);
    const auto wire = report_bytes(host, false);
    expect_failure(host, "wall", 2 * limit_ms);
    require(host.callbacks == host.spin_at && wire.find("\"trailer\"") == std::string::npos,
            "budget must stop at an input boundary and abandon the sink trailer");
}

void unchanged_row() {
    ClockScope clock;
    Host plain, bounded, unlimited, removed;
    install(bounded, limit_ms, limit_ms);
    bounded.at_limit = true;
    install(unlimited, 0, 0);
    unlimited.spin = true;
    install(removed, limit_ms, limit_ms);
    require(strategy_set_run_budget_v1(removed.handle(), nullptr) == 0, "remove budget");
    removed.spin = true;
    const auto expected = report_bytes(plain);
    require(cpu_reads == 0 && wall_reads == 0, "absent budget must not sample resource clocks");
    require(report_bytes(bounded) == expected && cpu_reads > 0,
            "unreached budget must exercise clocks and preserve every report byte and hash");
    const int measured = cpu_reads;
    const int measured_wall = wall_reads;
    require(report_bytes(unlimited) == expected && report_bytes(removed) == expected
                && cpu_reads == measured && wall_reads == measured_wall,
            "zero and removed budgets must preserve bytes without sampling resource clocks");
}

void setter_row() {
    require(pf_run_budget_version && pf_run_budget_version() == 1, "run budget version is absent");
    Host host;
    install(host, limit_ms, 0);
    Budget bad{};
    bad.struct_size = sizeof bad; bad.version = 2;
    require(strategy_set_run_budget_v1(host.handle(), &bad) == -1, "refuse bad budget version");
    bad.version = 1; --bad.struct_size;
    require(strategy_set_run_budget_v1(host.handle(), &bad) == -1, "refuse bad budget size");
    bad.struct_size = sizeof bad;
    bad.wall_ms = std::numeric_limits<std::uint64_t>::max();
    require(strategy_set_run_budget_v1(host.handle(), &bad) == -1, "refuse unrepresentable budget");
    bad.wall_ms = 0;
    bad.cpu_ms = std::numeric_limits<std::uint64_t>::max();
    require(strategy_set_run_budget_v1(host.handle(), &bad) == -1, "refuse unrepresentable CPU budget");
    bad.cpu_ms = 0;
    bad.sink_stall_ms = std::numeric_limits<std::uint64_t>::max();
    require(strategy_set_run_budget_v1(host.handle(), &bad) == -1, "refuse unrepresentable sink budget");
    require(strategy_set_run_budget_v1(nullptr, nullptr) == -1, "refuse null budget handle");
    host.spin = true;
    host.execute();
    expect_failure(host, "wall", 2 * limit_ms);
}

void layout_row() {
    // Two 32-bit header words followed by three 64-bit limits: 8+3*8=32.
    constexpr auto header_bytes = 2 * sizeof(std::uint32_t);
    constexpr auto limit_bytes = sizeof(std::uint64_t);
    require(sizeof(pf_run_budget_v1) == header_bytes + 3 * limit_bytes,
            "run budget descriptor must contain three limits in 32 bytes");
    Budget budget{};
    const auto* base = reinterpret_cast<const char*>(&budget);
    require(reinterpret_cast<const char*>(&budget.wall_ms) - base == header_bytes
                && reinterpret_cast<const char*>(&budget.cpu_ms) - base == header_bytes + limit_bytes
                && reinterpret_cast<const char*>(&budget.sink_stall_ms) - base
                    == header_bytes + 2 * limit_bytes,
            "run budget limit offsets must be 8, 16 and 24");
    require(budget.wall_ms == 0 && budget.cpu_ms == 0 && budget.sink_stall_ms == 0,
            "all three budget defaults must be off");
    Host host;
    install(host, 0, 0, limit_ms);
    budget.struct_size = header_bytes + 2 * limit_bytes;
    budget.version = 1;
    require(strategy_set_run_budget_v1(host.handle(), &budget) == -1,
            "unreleased descriptor must reject the old 24-byte size");
}

void real_spin_row() {
    Host host;
    host.spin = host.real_clock = true;
    install(host, 0, limit_ms);
    host.execute();
    require(host.last_run_status() == 1 && host.callbacks == 1,
            "real CPU spin must stop at the next bar boundary");
    require(std::string(strategy_get_last_error_code(host.handle())) == "run_budget_exceeded",
            "real CPU spin must have a typed failure");
    long long used = 0;
    const std::string args = strategy_get_last_error_args(host.handle());
    require(std::sscanf(args.c_str(), "{\"kind\":\"cpu\",\"limit\":100,\"used\":%lld}", &used) == 1
                && used >= 2 * limit_ms
                && args == "{\"kind\":\"cpu\",\"limit\":" + std::to_string(limit_ms)
                    + ",\"used\":" + std::to_string(used) + "}",
            "real CPU failure must preserve the host limit and measured CPU");
}

void timed_out(int) {
    constexpr char message[] = "run budget test exceeded its derived timeout\n";
    const auto ignored = ::write(STDERR_FILENO, message, sizeof message - 1);
    (void)ignored;
    ::_exit(124);
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(::signal(SIGALRM, timed_out) != SIG_ERR, "install budget test timeout");
        ::alarm(timeout_seconds);
        const std::string row = argc > 1 ? argv[1] : "wall";
        if (row == "wall") spin_row(false, false);
        else if (row == "cpu") spin_row(true, false);
        else if (row == "begin_callback") spin_row(false, true);
        else if (row == "begin") begin_row();
        else if (row == "unchanged") unchanged_row();
        else if (row == "setter") { ClockScope clock; setter_row(); }
        else if (row == "layout") layout_row();
        else if (row == "sink") sink_row();
        else if (row == "real_cpu") real_spin_row();
        else throw std::runtime_error("unknown budget row");
        ::alarm(0);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
