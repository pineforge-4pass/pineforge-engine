// RED-FIRST: frozen pineforge.h lacks pf_run_budget_install_v1.
// Expected boundaries and result counts below follow the fixture's rules.
#include "run_sigterm_abort_fixture.hpp"
#include "report_sink_failure_fixture.hpp"
#include <pineforge/pineforge.h>

#include <chrono>
#include <limits>
#include <thread>
#include <time.h>

namespace {
using cr150_w1::require;
using namespace std::chrono_literals;

struct Reset {
    ~Reset() { pf_run_budget_install_v1(0, 0); }
};

class Sink {
public:
    Sink() {
        require(::pipe(fds_) == 0, "create budget sink");
        reader_ = std::thread([this] {
            char buffer[8192];
            for (;;) {
                const auto count = ::read(fds_[0], buffer, sizeof buffer);
                if (count < 0 && errno == EINTR) continue;
                if (count < 0) { error_ = "budget sink read failed"; break; }
                if (count == 0) break;
                bytes.append(buffer, static_cast<std::size_t>(count));
            }
            ::close(fds_[0]);
        });
    }
    ~Sink() { finish(); }
    void install(pineforge::BacktestEngine& host) {
        const pf_report_sink_fd_v1 sink{sizeof sink, 1, fds_[1], "budget-test"};
        require(strategy_set_report_sink_fd_v1(&host, &sink) == 0, "install budget sink");
    }
    void finish() {
        if (fds_[1] < 0) return;
        ::close(fds_[1]);
        fds_[1] = -1;
        reader_.join();
    }
    void check(bool complete) {
        finish();
        require(error_.empty(), error_);
        require(!bytes.empty() && bytes.back() == '\n', "sink ends on a complete record");
        require((bytes.find("\"trailer\":") != std::string::npos) == complete,
                "budget failure has no trailer; controls have a trailer");
    }
    std::string bytes;
private:
    int fds_[2]{-1, -1};
    std::thread reader_;
    std::string error_;
};

class Host final : public q7_fixture::Host {
public:
    const char* kind = nullptr;
    bool returned_from_delay = false;
    bool install_in_begin = false;
    void on_native_run_begin() override {
        q7_fixture::Host::on_native_run_begin();
        // Clock epoch immediately before bar 0: no setup latency can choose
        // an earlier boundary. There are no budget checks inside a callback.
        if (install_in_begin)
            require(pf_run_budget_install_v1(kind[0] == 'w' ? 50 : 0,
                                            kind[0] == 'c' ? 10 : 0) == 0,
                    "install measured budget");
    }
protected:
    void after_identity(int index) override {
        if (!kind || index != 0) return;
        if (kind[0] == 'w') {
            std::this_thread::sleep_for(100ms); // 100 >= 50; boundary 1 must stop
        } else {
            timespec start{}, now{};
            require(::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &start) == 0,
                    "read fixture CPU start");
            do {
                // Real CPU work, independent of wall time and engine clocks.
                volatile std::uint64_t value = 1;
                for (int i = 0; i < 10000; ++i) value = value * 1664525 + 1013904223;
                require(::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now) == 0,
                        "read fixture CPU progress");
            } while ((now.tv_sec - start.tv_sec) * INT64_C(1000000000)
                         + now.tv_nsec - start.tv_nsec < INT64_C(20000000));
        }
        returned_from_delay = true;
        require(last_run_status() == 0, "budget must not interrupt the in-flight callback");
    }
};

std::vector<pineforge::Bar> feed() {
    std::vector<pineforge::Bar> bars;
    for (int i = 0; i < 6; ++i)
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1.0, i * INT64_C(60000)});
    return bars;
}

void breach(const char* kind) {
    Reset reset;
    require(pf_run_budget_install_v1(0, 0) == 0, "start with no budget");
    Sink sink;
    Host host;
    host.kind = kind;
    host.install_in_begin = true;
    host.configure("1", "1");
    sink.install(host);
    const auto bars = feed();
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(host.returned_from_delay && host.callbacks == 1 && host.applied == 1,
            "exactly bar 0's callback and close match complete before budget failure");
    require(host.last_run_status() == 1, "budget must fail the run before bar 1");
    require(std::string(strategy_get_last_error_code(&host)) == "run_budget_exceeded",
            "budget publishes its own terminal failure even for a Quiet host");
    const auto failure = pineforge::run_failure_value_of(host);
    require(pineforge::run_failure_code_class(failure.code) == pineforge::RunFailureClass::resource
                && !pineforge::run_failure_code_retryable(failure.code),
            "budget is a nonretryable resource outcome");
    const std::int64_t limit = kind[0] == 'w' ? 50 : 10;
    const std::string prefix = std::string("{\"kind\":\"") + kind
        + "\",\"limit\":" + std::to_string(limit) + ",\"used\":";
    const std::string args = strategy_get_last_error_args(&host);
    require(args.compare(0, prefix.size(), prefix) == 0 && args.back() == '}',
            "kind and limit are typed and exact");
    const auto tail = args.substr(prefix.size());
    std::size_t parsed = 0;
    const auto used = std::stoll(tail, &parsed);
    require(parsed + 1 == tail.size() && tail.back() == '}' && used >= limit,
            "used is an integer number of milliseconds meeting the configured limit");
    pineforge::ReportC report{};
    host.fill_report(&report);
    require(report.input_bars_processed == 1 && report.script_bars_processed == 1,
            "failure is observed at the first completed input-bar boundary");
    pineforge::BacktestEngine::free_report(&report);
    require(std::string(strategy_get_last_error_args(&host)) == args,
            "report capture preserves the terminal budget witness");
    pf_report_sink_receipt_v1 receipt{sizeof receipt, 1};
    require(strategy_report_sink_receipt_v1(&host, &receipt) == 0 && !receipt.complete,
            "budget failure cannot complete the sink receipt");
    sink.check(false);
}

std::string control(std::uint64_t budget) {
    Reset reset;
    require(pf_run_budget_install_v1(budget, budget) == 0, "install control budget");
    Sink sink;
    Host host;
    host.configure("1", "1");
    sink.install(host);
    const auto bars = feed();
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(host.last_run_status() == 0 && host.last_error().empty(), "control completes");
    require(host.callbacks == 6 && host.applied == 6, "all six control bars match at close");
    pineforge::ReportC report{};
    host.fill_report(&report);
    require(report.input_bars_processed == 6 && report.script_bars_processed == 6
                && report.total_trades == 3 && report.trades_len == 0
                && report.net_profit == 0.0 && strategy_position_size(&host) == 0.0,
            "alternating six one-unit transactions close three trades");
    pineforge::BacktestEngine::free_report(&report);
    pf_report_sink_receipt_v1 receipt{sizeof receipt, 1};
    require(strategy_report_sink_receipt_v1(&host, &receipt) == 0 && receipt.complete
                && receipt.count_e == 6 && receipt.count_t == 3,
            "control sink has all six equity points and three trades");
    sink.check(true);
    return sink.bytes; // compare complete canonical output, including its digest
}
} // namespace

int main() {
    try {
        cr150_w1::Environment arena_fd("PF_SCRATCH_ARENA_FD", nullptr);
        cr150_w1::Environment arena_bytes("PF_SCRATCH_ARENA_BYTES", nullptr);
        require(pf_run_budget_install_v1(std::numeric_limits<std::uint64_t>::max(), 0) == -1,
                "out-of-range limit is refused");
        breach("wall");
        breach("cpu");
        require(control(0) == control(600000), "disabled and generous budgets give identical results");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_run_budget: %s\n", error.what());
        return 1;
    }
}
