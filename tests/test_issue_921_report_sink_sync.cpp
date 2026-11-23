#include "report_sink_failure_fixture.hpp"

#include <pineforge/native_host.hpp>

#include <fstream>
#include <string_view>

namespace {

using namespace cr150_w1;

constexpr int kBars = 16384;
constexpr std::string_view kHeader = R"({"schema":"pf-report-sink","version":1,"run_id":"W1",)";
constexpr std::string_view kTrailer = R"({"trailer":"pf-report-sink",)";

constexpr std::int64_t tape_time(int i) {
    return INT64_C(1704067200000) + i * INT64_C(60000);
}

struct SyncProbe {
    dev_t device{};
    ino_t inode{};
    bool armed = false;
    unsigned calls = 0;
    int injected_errno = 0;
} probe;

struct Scratch {
    std::string directory;
    std::string path;

    Scratch() {
        const char* temporary = std::getenv("TMPDIR");
        directory = std::string(temporary && *temporary ? temporary : "/tmp")
            + "/pineforge-issue-921-XXXXXX";
        if (::mkdtemp(directory.data()) == nullptr)
            throw std::system_error(errno, std::generic_category(), "create sink directory");
        path = directory + "/fault.sink";
    }
    ~Scratch() {
        ::unlink(path.c_str());
        ::rmdir(directory.c_str());
    }
};

class QuietHost final : public pineforge::NativeStrategyHost {
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {}
};

int not_compared(const char* reason, std::string_view actual, std::string_view expected) {
    std::fprintf(stderr, "NOT COMPARED #921: %s; actual=%.*s; expected=%.*s\n", reason,
                 static_cast<int>(actual.size()), actual.data(),
                 static_cast<int>(expected.size()), expected.data());
    return 1;
}

int check() {
    Environment arena_fd("PF_SCRATCH_ARENA_FD", nullptr);
    Environment arena_bytes("PF_SCRATCH_ARENA_BYTES", nullptr);
    Environment window("PF_SCRATCH_WINDOW_BYTES", nullptr);
    Scratch scratch;
    QuietHost host;
    pineforge::NativeRunSpec spec;
    spec.identity = {"deferred-sink-sync", 1};
    spec.input_tf = spec.script_tf = "1";
    spec.tickerid = "TEST:SINKSYNC";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 1000.0;
    spec.point_value = spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.report_policy = pineforge::NativeReportPolicy::KernelRecorded;
    const auto setup = host.configure_native(spec).status;
    if (setup != pineforge::NativeSetupStatus::Applied)
        return not_compared("native setup", std::to_string(static_cast<int>(setup)), "0");

    {
        Fd file(::open(scratch.path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        struct stat info{};
        if (file.value < 0 || ::fstat(file.value, &info) != 0)
            throw std::system_error(errno, std::generic_category(), "identify sink file");
        probe = {info.st_dev, info.st_ino, true, 0, 0};
    }
    install_path(host, scratch.path, scratch.directory);
    const auto bars = tape(kBars);
    if (bars.size() != static_cast<std::size_t>(kBars))
        return not_compared("input bar count", std::to_string(bars.size()), std::to_string(kBars));
    for (int i = 0; i < kBars; ++i) {
        const auto& bar = bars[static_cast<std::size_t>(i)];
        if (bar.open == 100.0 && bar.high == 101.0 && bar.low == 99.0 && bar.close == 100.0
                && bar.volume == 1.0 && bar.timestamp == tape_time(i))
            continue;
        char actual[160];
        std::snprintf(actual, sizeof actual, "%.17g,%.17g,%.17g,%.17g,%.17g,%lld",
                      bar.open, bar.high, bar.low, bar.close, bar.volume,
                      static_cast<long long>(bar.timestamp));
        return not_compared("input OHLCV and time", actual,
                            "100,101,99,100,1," + std::to_string(tape_time(i)));
    }
    run(host, bars);
    if (host.last_run_status() != 0 || !host.last_error().empty())
        return not_compared("run before report fill",
                            std::to_string(host.last_run_status()) + ": " + host.last_error(),
                            "0 with no error");
    if (probe.calls != 0)
        return not_compared("sink syncs before report fill", std::to_string(probe.calls), "0");

    pineforge::ReportC report{};
    std::string outcome = "returned normally";
    try {
        host.fill_report(&report);
    } catch (const std::exception& error) {
        const auto failure = pineforge::classify_run_failure(error);
        outcome = std::string(pineforge::run_failure_code_name(failure.code)) + " " + args(failure);
    }
    pineforge::BacktestEngine::free_report(&report);
    if (probe.injected_errno != EIO)
        return not_compared("injected errno on the first sink fdatasync",
                            std::to_string(probe.injected_errno), std::to_string(EIO));
    constexpr auto expected_failure = R"(report_sink_failed {"reason":"sink_io"})";
    if (outcome != expected_failure)
        return not_compared("report fill outcome", outcome, expected_failure);

    std::ifstream input(scratch.path, std::ios::binary);
    if (!input) return not_compared("sink read", "open failed", "readable file");
    std::string line;
    if (!std::getline(input, line))
        return not_compared("sink header", "<missing>", kHeader);
    if (line.compare(0, kHeader.size(), kHeader) != 0)
        return not_compared("sink header prefix", line, kHeader);
    for (int i = 0; i < kBars; ++i) {
        const auto expected = "[\"E\"," + std::to_string(tape_time(i)) + ",1000.0,0.0]";
        if (!std::getline(input, line))
            return not_compared("equity record", "<missing>", expected);
        if (line != expected || input.eof())
            return not_compared("equity record", line + (input.eof() ? " <no LF>" : ""),
                                expected + " followed by LF");
    }
    if (input.peek() != std::char_traits<char>::eof()) {
        std::getline(input, line);
        std::fprintf(stderr, "FAIL #921: after sink sync EIO, actual=%s; expected=EOF\n",
                     line.compare(0, kTrailer.size(), kTrailer) == 0
                         ? "pf-report-sink completion trailer" : "unexpected bytes");
        return 1;
    }
    if (input.bad() || !input.eof())
        return not_compared("end of sink file", "read failed", "EOF");
    std::puts("PASS #921: sink sync EIO left 16384 equity records and no trailer");
    return 0;
}

} // namespace

extern "C" int __real_fdatasync(int);
extern "C" int __wrap_fdatasync(int fd) {
    struct stat info{};
    if (probe.armed && ::fstat(fd, &info) == 0
            && info.st_dev == probe.device && info.st_ino == probe.inode
            && ++probe.calls == 1) {
        probe.injected_errno = EIO;
        errno = EIO;
        return -1;
    }
    return __real_fdatasync(fd);
}

int main() {
    try {
        return check();
    } catch (const std::exception& error) {
        return not_compared("fixture setup or execution", error.what(), "fixture ready to compare");
    }
}
