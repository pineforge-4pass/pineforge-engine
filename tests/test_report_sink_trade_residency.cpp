// The sink keeps the logical T result complete while moving closed rows out of
// the process heap. This witness compares every trade population with a
// same-bar zero-trade control and checks both sink transports.
//
// The bar the heap is held to has two parts. A sink run may hold the ledger's
// declared read-cache cap (TradeLedger::kSinkCacheRows rows of
// kLedgerEntryBytes each, read from the engine's own constants) above its
// zero-trade control, plus the fixed slack below. And that excess must not
// move with the number of trades: the populations below reach the cap and go
// far past it, and their excess over the control has to agree within the slack.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include "../src/native_execution_consumer.hpp"

#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/report_sink.h>
#include <pineforge/report_sink_writer.hpp>
#include <pineforge/trade_ledger.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace pineforge;
namespace no = pineforge::native_order;

constexpr std::int64_t kStartMs = INT64_C(1704067200000);
constexpr std::int64_t kMinuteMs = INT64_C(60000);
constexpr std::size_t kHeapSlack = 2 * report_sink::kScratchWindowBytes + 512 * 1024;
// The declared read-cache cap of a sink run, in bytes, from the ledger's own
// constants: the cache allocation never exceeds this however many trades close.
const std::size_t kSinkCacheCapBytes =
    TradeLedger::resident_cache_bytes(TradeLedger::kSinkCacheRows);

// What one population leaves above its same-bar control, per transport.
struct Excess {
    std::size_t path_peak = 0;
    std::size_t fd_peak = 0;
};

std::size_t above(std::size_t value, std::size_t control) {
    return value > control ? value - control : 0;
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Directory {
    std::string path;

    Directory() {
        const char* root = std::getenv("TMPDIR");
        path = root && *root ? root : "/tmp";
        path += "/pineforge-report-residency-XXXXXX";
        require(::mkdtemp(path.data()) != nullptr, "cannot create report residency directory");
    }

    ~Directory() {
        ::rmdir(path.c_str());
    }

    std::string file(const char* name) const {
        return path + "/" + name;
    }
};

void write_all(int fd, const char* data, std::size_t size, int& error) noexcept {
    std::size_t written = 0;
    while (written < size) {
        const ssize_t result = ::write(fd, data + written, size - written);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) {
            error = result == 0 ? EIO : errno;
            return;
        }
        written += static_cast<std::size_t>(result);
    }
}

// A reader for the borrowed-fd route. It uses only fixed buffers while the
// production allocation window is active, so the witness does not retain the
// output it is measuring.
class FdCapture {
public:
    explicit FdCapture(std::string output_path) : output_path_(std::move(output_path)) {
        int pair[2]{-1, -1};
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
                "cannot create report fd socket");
        reader_ = pair[0];
        const int duplicate = ::fcntl(pair[1], F_DUPFD_CLOEXEC, 32);
        ::close(pair[1]);
        require(duplicate >= 0, "cannot move report fd above reserved descriptors");
        writer_ = duplicate;
        output_ = ::open(output_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        require(output_ >= 0, "cannot create fd sink capture");
        thread_ = std::thread([this] {
            std::array<char, 64 * 1024> buffer{};
            ready_.store(true, std::memory_order_release);
            for (;;) {
                const ssize_t count = ::read(reader_, buffer.data(), buffer.size());
                if (count < 0 && errno == EINTR) continue;
                if (count < 0) {
                    error_.store(errno, std::memory_order_relaxed);
                    break;
                }
                if (count == 0) break;
                int error = 0;
                write_all(output_, buffer.data(), static_cast<std::size_t>(count), error);
                if (error != 0) {
                    error_.store(error, std::memory_order_relaxed);
                    break;
                }
            }
        });
        while (!ready_.load(std::memory_order_acquire)) std::this_thread::yield();
    }

    ~FdCapture() {
        close_writer();
        if (thread_.joinable()) thread_.join();
        if (reader_ >= 0) ::close(reader_);
        if (output_ >= 0) ::close(output_);
    }

    int writer() const noexcept { return writer_; }

    void finish() {
        close_writer();
        if (thread_.joinable()) thread_.join();
        require(error_.load(std::memory_order_relaxed) == 0, "fd sink reader failed");
        require(::fsync(output_) == 0, "fd sink capture flush failed");
    }

    const std::string& path() const noexcept { return output_path_; }

private:
    void close_writer() noexcept {
        if (writer_ < 0) return;
        ::shutdown(writer_, SHUT_WR);
        ::close(writer_);
        writer_ = -1;
    }

    std::string output_path_;
    int reader_ = -1;
    int writer_ = -1;
    int output_ = -1;
    std::atomic<bool> ready_{false};
    std::atomic<int> error_{0};
    std::thread thread_;
};

struct BarTape {
    std::vector<Bar> bars;

    explicit BarTape(int trade_count) {
        // Bar zero submits, bar one opens, and each later filled reversal
        // closes one row. The final open lot is deliberately not reported.
        const std::size_t count = static_cast<std::size_t>(trade_count) + 2;
        bars.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            const double price = 100.0 + static_cast<double>(index % 17) * 0.25;
            bars.push_back({price, price, price, price, 1.0,
                            kStartMs + static_cast<std::int64_t>(index) * kMinuteMs});
        }
    }
};

NativeRunSpec spec() {
    NativeRunSpec value;
    value.identity = {"report-sink-residency", 1};
    value.input_tf = value.script_tf = "1";
    value.tickerid = "TEST:REPORT";
    value.timezone = "UTC";
    value.session = "24x7";
    value.initial_capital = 1000000.0;
    value.point_value = value.account_fx = 1.0;
    value.price_tick = 0.01;
    value.report_policy = NativeReportPolicy::KernelRecorded;
    value.report_open_position_at_end = false;
    return value;
}

class ReversalHost final : public NativeStrategyHost {
public:
    explicit ReversalHost(bool produce_trades) : produce_trades_(produce_trades) {}

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (produce_trades_) {
            const double quantity = bar_index_ == 0
                ? 1.0
                : (bar_index_ % 2 == 0 ? 2.0 : -2.0);
            const auto result = submit({no::Transact{quantity}, "flip", ""});
            require(result.status == no::SubmitStatus::Accepted
                        && result.handle.has_value(),
                    "reversal order was refused");
        }
        ++bar_index_;
    }

    std::size_t resident_trade_rows() const { return trades_.resident_size(); }
    std::size_t resident_cache_bytes() const { return trades_.cache_resident_bytes(); }
    std::size_t resident_checkpoints() const {
        return NativeExecutionConsumer::bound(*this).resident_closed_row_checkpoints();
    }

private:
    bool produce_trades_;
    int bar_index_ = 0;
};

struct ArenaSnapshot {
    bool attached = false;
    std::uint64_t bytes_used = 0;
    std::uint64_t high_water = 0;
};

ArenaSnapshot arena_snapshot() {
    const auto* arena = detail::ScratchArena::attached();
    if (!arena) return {};
    const auto stats = arena->stats();
    return {true, stats.bytes_used, stats.high_water};
}

struct AllocationWindow {
    std::size_t peak = 0;
};

AllocationWindow* active_window = nullptr;

void record_allocation(std::size_t) {
    if (active_window)
        active_window->peak = std::max(active_window->peak, global_allocation::live_bytes);
}

struct ReportOwner {
    ReportC value{};
    ~ReportOwner() { BacktestEngine::free_report(&value); }
};

struct Sample {
    std::size_t peak = 0;
    std::size_t live = 0;
    std::size_t resident_rows = 0;
    std::size_t cache_bytes = 0;
    std::size_t checkpoints = 0;
    std::size_t trades = 0;
    std::size_t bars = 0;
    ArenaSnapshot before_arena{};
    ArenaSnapshot after_arena{};
    pf_metrics_t metrics{};
    pf_report_sink_receipt_v1 receipt{};
    pf_report_sink_summary_v1 summary{};
    std::string output_path;
};

Sample run_sample(const Directory& directory, int population, bool produce_trades,
                  bool fd_route, const BarTape& tape, const char* label) {
    ReversalHost host(produce_trades);
    require(host.configure_native(spec()).status == NativeSetupStatus::Applied,
            "native fixture configure refused");
    host.set_broker_state_hash_recording(true);

    std::unique_ptr<FdCapture> capture;
    std::string output_path;
    if (fd_route) {
        output_path = directory.file(label);
        capture = std::make_unique<FdCapture>(output_path);
        const pf_report_sink_fd_v1 sink{sizeof sink, 1, capture->writer(), "residency"};
        require(host.set_report_sink_fd_v1(&sink) == 0, "fd sink install refused");
    } else {
        output_path = directory.file(label);
        const pf_report_sink_v1 sink{
            sizeof sink, 1, output_path.c_str(), "residency", directory.path.c_str()};
        require(host.set_report_sink_v1(&sink) == 0, "path sink install refused");
    }

    Sample result;
    result.output_path = output_path;
    result.bars = tape.bars.size();
    result.before_arena = arena_snapshot();

    AllocationWindow window;
    active_window = &window;
    global_allocation::live_bytes = 0;
    global_allocation::on_recorded = record_allocation;
    global_allocation::recording = true;
    try {
        host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
        ReportOwner report;
        host.fill_report(&report.value);
        result.metrics = report.value.metrics;
        result.trades = static_cast<std::size_t>(report.value.total_trades);
        require(report.value.trades == nullptr && report.value.trades_len == 0,
                "sink report retained a TradeC array");
        result.receipt.struct_size = sizeof result.receipt;
        result.receipt.version = 1;
        require(host.report_sink_receipt_v1(&result.receipt) == 0,
                "sink receipt query refused");
        result.summary.struct_size = sizeof result.summary;
        result.summary.version = 1;
        require(host.report_sink_summary_v1(&result.summary) == 0,
                "sink summary query refused");
        result.resident_rows = host.resident_trade_rows();
        result.cache_bytes = host.resident_cache_bytes();
        result.checkpoints = host.resident_checkpoints();
        result.after_arena = arena_snapshot();
        result.live = global_allocation::live_bytes;
    } catch (...) {
        global_allocation::recording = false;
        global_allocation::on_recorded = nullptr;
        active_window = nullptr;
        throw;
    }
    result.peak = window.peak;
    global_allocation::recording = false;
    global_allocation::on_recorded = nullptr;
    active_window = nullptr;

    if (capture) capture->finish();
    require(host.last_run_status() == 0 && host.last_error().empty(),
            "sink residency run failed");
    require(result.receipt.complete == 1 && result.summary.complete == 1,
            "sink did not publish a complete receipt and summary");
    require(result.receipt.count_t == static_cast<std::int64_t>(
                produce_trades ? population : 0),
            "sink T count differs from the independent population");
    require(result.receipt.count_e == static_cast<std::int64_t>(result.bars),
            "sink E count differs from the input bar count");
    require(result.receipt.count_h == static_cast<std::int64_t>(result.bars),
            "sink H count differs from the input bar count");
    require(result.summary.count_t == result.receipt.count_t,
            "sink summary count differs from the trailer count");
    require(result.trades == static_cast<std::size_t>(
                produce_trades ? population : 0),
            "logical report trade count differs from the independent population");
    require(result.resident_rows <= TradeLedger::kResidentRows,
            "sink ledger retained more than its descriptor window");
    require(result.cache_bytes <=
                TradeLedger::resident_cache_bytes(TradeLedger::kSinkCacheRows),
            "sink ledger read cache exceeded its fixed budget");
    require(result.checkpoints <= 64, "sink digest checkpoints grew with trades");
    if (result.after_arena.attached) {
        require(result.after_arena.high_water >= result.before_arena.high_water,
                "arena high-water counter moved backwards");
        require(result.after_arena.bytes_used >= result.before_arena.bytes_used,
                "arena extent counter moved backwards");
    }
    return result;
}

std::uintmax_t file_size(const std::string& path) {
    struct stat info{};
    require(::stat(path.c_str(), &info) == 0, "cannot stat sink output");
    return static_cast<std::uintmax_t>(info.st_size);
}

void compare_files(const std::string& left, const std::string& right) {
    require(file_size(left) == file_size(right), "path and fd sink sizes differ");
    const int left_fd = ::open(left.c_str(), O_RDONLY | O_CLOEXEC);
    const int right_fd = ::open(right.c_str(), O_RDONLY | O_CLOEXEC);
    require(left_fd >= 0 && right_fd >= 0, "cannot open sink outputs for comparison");
    std::array<char, 64 * 1024> left_buffer{};
    std::array<char, 64 * 1024> right_buffer{};
    for (;;) {
        const ssize_t left_count = ::read(left_fd, left_buffer.data(), left_buffer.size());
        const ssize_t right_count = ::read(right_fd, right_buffer.data(), right_buffer.size());
        require(left_count >= 0 && right_count >= 0, "cannot read sink outputs");
        require(left_count == right_count, "path and fd sink reads differ");
        if (left_count == 0) break;
        require(std::memcmp(left_buffer.data(), right_buffer.data(),
                            static_cast<std::size_t>(left_count)) == 0,
                "path and fd sink bytes differ");
    }
    ::close(left_fd);
    ::close(right_fd);
}

void skip_integer(const char*& cursor) {
    char* end = nullptr;
    (void)std::strtoll(cursor, &end, 10);
    require(end != cursor && *end == ',', "malformed version-1 integer");
    cursor = end + 1;
}

double take_double(const char*& cursor) {
    char* end = nullptr;
    const double value = std::strtod(cursor, &end);
    require(end != cursor && *end == ',' && std::isfinite(value),
            "malformed version-1 finite double");
    cursor = end + 1;
    return value;
}

struct WireEvidence {
    pf_report_sink_summary_v1 summary{};
    std::size_t equity = 0;
    std::size_t trades = 0;
    std::size_t hashes = 0;
    std::size_t trailers = 0;
};

WireEvidence check_wire_shape(const std::string& path, int population, std::size_t bars) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "cannot open sink wire for shape check");
    std::string line;
    WireEvidence evidence;
    evidence.summary.struct_size = sizeof evidence.summary;
    evidence.summary.version = 1;
    evidence.summary.best_trade = 0.0;
    evidence.summary.worst_trade = 0.0;
    std::int64_t next_trade = 1;
    double cumulative = 0.0;
    double peak = 0.0;
    while (std::getline(input, line)) {
        if (line.rfind("[\"E\",", 0) == 0) {
            ++evidence.equity;
        } else if (line.rfind("[\"T\",", 0) == 0) {
            ++evidence.trades;
            const auto comma = line.find(',', 5);
            require(comma != std::string::npos, "malformed version-1 T row");
            const auto number = std::strtoll(line.c_str() + 5, nullptr, 10);
            require(number == next_trade++, "version-1 T rows are not contiguous");
            const char* cursor = line.c_str() + comma + 1;
            require(*cursor == '"', "malformed version-1 T side");
            const char* side_end = std::strchr(cursor + 1, '"');
            require(side_end != nullptr && side_end[1] == ',',
                    "malformed version-1 T side");
            cursor = side_end + 2;
            skip_integer(cursor);
            skip_integer(cursor);
            (void)take_double(cursor);
            (void)take_double(cursor);
            (void)take_double(cursor);
            const double pnl = take_double(cursor);
            if (evidence.summary.count_t == 0) {
                evidence.summary.best_trade = evidence.summary.worst_trade = pnl;
            } else {
                evidence.summary.best_trade = std::max(evidence.summary.best_trade, pnl);
                evidence.summary.worst_trade = std::min(evidence.summary.worst_trade, pnl);
            }
            ++evidence.summary.count_t;
            if (pnl > 0.0) ++evidence.summary.wins;
            if (pnl < 0.0) ++evidence.summary.losses;
            // The v1 summary's drawdown fold is over original T P&Ls.
            cumulative += pnl;
            peak = std::max(peak, cumulative);
            evidence.summary.max_drawdown =
                std::min(evidence.summary.max_drawdown, cumulative - peak);
        } else if (line.rfind("[\"H\",", 0) == 0) {
            ++evidence.hashes;
            require(line.size() == 24 && line[4] == ',' && line[5] == '"'
                        && line[22] == '"' && line[23] == ']',
                    "malformed version-1 H row");
            for (std::size_t index = 6; index < 22; ++index)
                require((line[index] >= '0' && line[index] <= '9')
                            || (line[index] >= 'a' && line[index] <= 'f'),
                        "version-1 H row is not lowercase hex");
        } else if (line.rfind("{\"trailer\":", 0) == 0) {
            ++evidence.trailers;
        }
    }
    require(input.eof(), "sink wire read failed");
    require(evidence.equity == bars && evidence.hashes == bars
                && evidence.trades == static_cast<std::size_t>(population),
            "version-1 wire counts differ from the independent model");
    require(evidence.trailers == 1, "version-1 wire has no single final trailer");
    evidence.summary.complete = 1;
    return evidence;
}

void compare_samples(const Sample& path, const Sample& fd, int population) {
    compare_files(path.output_path, fd.output_path);
    require(std::memcmp(&path.receipt, &fd.receipt, sizeof path.receipt) == 0,
            "path and fd receipts differ");
    require(std::memcmp(&path.summary, &fd.summary, sizeof path.summary) == 0,
            "path and fd summaries differ");
    require(std::memcmp(&path.metrics, &fd.metrics, sizeof path.metrics) == 0,
            "path and fd report metrics differ");
    require(path.trades == static_cast<std::size_t>(population)
                && fd.trades == path.trades,
            "path and fd logical trade totals differ");
}

void remove_output(const Sample& sample) {
    require(::unlink(sample.output_path.c_str()) == 0, "cannot remove sink output");
}

Excess run_population(const Directory& directory, int population) {
    const BarTape tape(population);
    const auto path_control = run_sample(directory, population, false, false, tape,
                                         "path-control.ndjson");
    const auto fd_control = run_sample(directory, population, false, true, tape,
                                       "fd-control.ndjson");
    const auto path_trades = run_sample(directory, population, true, false, tape,
                                        "path-trades.ndjson");
    const auto fd_trades = run_sample(directory, population, true, true, tape,
                                      "fd-trades.ndjson");

    // Print the four samples before judging them, so a failure names the sizes.
    const auto describe = [&](const char* label, const Sample& sample) {
        std::printf("report sink residency sample: trades=%d %s peak=%zu live=%zu "
                    "resident_rows=%zu cache_bytes=%zu slack=%zu\n",
                    population, label, sample.peak, sample.live, sample.resident_rows,
                    sample.cache_bytes, kHeapSlack);
    };
    describe("path_control", path_control);
    describe("fd_control", fd_control);
    describe("path_trades", path_trades);
    describe("fd_trades", fd_trades);

    // Above its control a sink run holds at most the declared cache cap.
    const std::size_t bound = kSinkCacheCapBytes + kHeapSlack;
    require(path_trades.peak <= path_control.peak + bound,
            "path sink heap peak exceeded the control plus the declared cache cap");
    require(fd_trades.peak <= fd_control.peak + bound,
            "fd sink heap peak exceeded the control plus the declared cache cap");
    require(path_trades.live <= path_control.live + bound,
            "path sink retained heap exceeded the control plus the declared cache cap");
    require(fd_trades.live <= fd_control.live + bound,
            "fd sink retained heap exceeded the control plus the declared cache cap");

    const auto path_wire = check_wire_shape(path_trades.output_path, population, tape.bars.size());
    const auto fd_wire = check_wire_shape(fd_trades.output_path, population, tape.bars.size());
    require(std::memcmp(&path_wire.summary, &path_trades.summary,
                        sizeof path_wire.summary) == 0,
            "path T summary differs from the independent wire fold");
    require(std::memcmp(&fd_wire.summary, &fd_trades.summary,
                        sizeof fd_wire.summary) == 0,
            "fd T summary differs from the independent wire fold");
    compare_samples(path_trades, fd_trades, population);

    remove_output(path_control);
    remove_output(fd_control);
    remove_output(path_trades);
    remove_output(fd_trades);
    std::printf("report sink residency: trades=%d path_peak=%zu fd_peak=%zu "
                "path_excess=%zu fd_excess=%zu cache_cap_bytes=%zu\n",
                population, path_trades.peak, fd_trades.peak,
                above(path_trades.peak, path_control.peak),
                above(fd_trades.peak, fd_control.peak), kSinkCacheCapBytes);
    return {above(path_trades.peak, path_control.peak),
            above(fd_trades.peak, fd_control.peak)};
}

} // namespace

int main() {
    try {
        Directory directory;
        // 100,000 trades already pass the cache cap's row count (65,536); 200,000
        // and 1,000,000 go 3x and 15x past it. Zero growth past the cap means
        // the excess over each population's own control agrees across them.
        std::vector<Excess> excess;
        for (const int population : {100000, 200000, 1000000})
            excess.push_back(run_population(directory, population));
        const auto spread = [&](std::size_t Excess::*member) {
            std::size_t low = excess.front().*member, high = low;
            for (const auto& item : excess) {
                low = std::min(low, item.*member);
                high = std::max(high, item.*member);
            }
            return high - low;
        };
        std::printf("report sink residency spread: path=%zu fd=%zu slack=%zu\n",
                    spread(&Excess::path_peak), spread(&Excess::fd_peak), kHeapSlack);
        require(spread(&Excess::path_peak) <= kHeapSlack,
                "path sink heap peak grew with the trade population past the cache cap");
        require(spread(&Excess::fd_peak) <= kHeapSlack,
                "fd sink heap peak grew with the trade population past the cache cap");
        return 0;
    } catch (const std::exception& error) {
        global_allocation::recording = false;
        global_allocation::on_recorded = nullptr;
        active_window = nullptr;
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
