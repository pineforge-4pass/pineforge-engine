// CX-SINK-B1/B2: real native-engine runs through the C ABI sink setters.
// Expected fail-before on a35a64c2: the fd descriptor/setter are undeclared.
// The supervisor records the actual frozen-header diagnostic on the lab.
#include <pineforge/pineforge.h>
#include <pineforge/report_sink.h>
#include <pineforge/native_host.hpp>
#include <pineforge/metrics_stream.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/detail/scratch_arena.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
using pineforge::BacktestEngine;
using pineforge::Bar;
using pineforge::NativeStrategyHost;
constexpr const char* run_id = "L22A \"stream\"\n\xc3\xa9";

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class Descriptor {
public:
    explicit Descriptor(int value = -1) : value_(value) {}
    ~Descriptor() { reset(); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    int get() const { return value_; }
    void reset() {
        if (value_ >= 0) ::close(std::exchange(value_, -1));
    }
private:
    int value_;
};

class SignalScope {
public:
    SignalScope(int number, void (*handler)(int)) : number_(number) {
        struct sigaction next{};
        next.sa_handler = handler;
        sigemptyset(&next.sa_mask);
        require(sigaction(number_, &next, &previous_) == 0, "cannot set test signal handler");
    }
    ~SignalScope() { sigaction(number_, &previous_, nullptr); }
    SignalScope(const SignalScope&) = delete;
    SignalScope& operator=(const SignalScope&) = delete;
private:
    int number_;
    struct sigaction previous_{};
};

void require_signal(int number, void (*handler)(int)) {
    struct sigaction current{};
    require(sigaction(number, nullptr, &current) == 0 && current.sa_handler == handler,
            "library changed a process-wide signal disposition");
}

// The test's reader retains bytes to compare them; the production writer
// keeps its existing fixed buffer. Closing the writer before join also makes
// exception unwinding safe if any engine assertion fails.
class Stream {
public:
    Stream(Descriptor reader, Descriptor writer, bool slow, std::size_t stop_after)
        : reader_(std::move(reader)), writer_(std::move(writer)),
          thread_([this, slow, stop_after] {
              try {
                  if (slow) std::this_thread::sleep_for(std::chrono::milliseconds(50));
                  std::array<char, 4096> buffer{};
                  while (!stop_after || bytes_.size() < stop_after) {
                      const auto room = stop_after
                          ? std::min(buffer.size(), stop_after - bytes_.size()) : buffer.size();
                      const auto count = ::read(reader_.get(), buffer.data(), room);
                      if (count < 0 && errno == EINTR) continue;
                      require(count >= 0, "stream read failed");
                      if (count == 0) break;
                      bytes_.append(buffer.data(), static_cast<std::size_t>(count));
                      if (slow) std::this_thread::sleep_for(std::chrono::milliseconds(2));
                  }
              } catch (const std::exception& error) {
                  error_ = error.what();
              }
              reader_.reset();
          }) {}
    ~Stream() {
        writer_.reset();
        if (thread_.joinable()) thread_.join();
    }
    int fd() const { return writer_.get(); }
    std::string finish() {
        writer_.reset();
        thread_.join();
        require(error_.empty(), error_);
        return std::move(bytes_);
    }
private:
    Descriptor reader_, writer_;
    std::string bytes_, error_;
    std::thread thread_;
};

enum class Transport { pipe, fifo, socket };

std::unique_ptr<Stream> stream(Transport transport, const fs::path& directory,
                               bool slow = false, std::size_t stop_after = 0) {
    if (transport == Transport::fifo) {
        const auto path = directory / "backpressure.fifo";
        require(::mkfifo(path.c_str(), 0600) == 0, "mkfifo failed");
        Descriptor reader(::open(path.c_str(), O_RDONLY | O_NONBLOCK));
        require(reader.get() >= 0, "cannot open FIFO reader");
        Descriptor writer(::open(path.c_str(), O_WRONLY | O_NONBLOCK));
        require(writer.get() >= 0, "cannot open FIFO writer");
        const int flags = ::fcntl(reader.get(), F_GETFL);
        require(flags >= 0 && ::fcntl(reader.get(), F_SETFL, flags & ~O_NONBLOCK) == 0,
                "cannot make FIFO reader blocking");
        require((::fcntl(writer.get(), F_GETFL) & O_NONBLOCK) != 0,
                "FIFO writer is not non-blocking");
        return std::make_unique<Stream>(std::move(reader), std::move(writer), slow, stop_after);
    }
    int descriptors[2]{-1, -1};
    require((transport == Transport::socket
                ? ::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) : ::pipe(descriptors)) == 0,
            "cannot create sink stream");
    Descriptor reader(descriptors[0]), original_writer(descriptors[1]);
    // The caller deliberately supplies a descriptor outside the wrapper's
    // conventional slots. Only this fixture duplicates it, never the library.
    Descriptor writer(::fcntl(original_writer.get(), F_DUPFD_CLOEXEC, 17));
    require(writer.get() >= 17, "cannot allocate a non-default inherited fd");
    original_writer.reset();
    if (transport == Transport::socket) {
        const int capacity = 4096;
        require(::setsockopt(writer.get(), SOL_SOCKET, SO_SNDBUF, &capacity, sizeof capacity) == 0,
                "cannot bound socket send buffer");
    }
    return std::make_unique<Stream>(std::move(reader), std::move(writer), slow, stop_after);
}

std::vector<Bar> feed() {
    std::vector<Bar> bars;
    for (int i = 0; i < 32; ++i) {
        const double price = 100.0 + i * 0.25;
        bars.push_back({price, price + 0.5, price - 0.5, price + 0.125,
                       1.0, INT64_C(1704067200000) + i * INT64_C(60000)});
    }
    return bars;
}

class Fixture final : public NativeStrategyHost {
public:
    Fixture() {
        declare_outputs(2, 1, 2);
        configure_run(1);
        require(set_outputs_enabled(true), "cannot enable outputs");
        set_trace_enabled(true);
        set_broker_state_hash_recording(true);
    }
    void configure_run(std::uint64_t run_number) {
        pineforge::NativeRunSpec spec;
        spec.identity = {"L22A", run_number};
        spec.input_tf = spec.script_tf = "1";
        spec.tickerid = "TEST:L22A";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.initial_capital = 10000.0;
        spec.point_value = spec.account_fx = 1.0;
        spec.price_tick = 0.01;
        spec.report_policy = pineforge::NativeReportPolicy::KernelRecorded;
        spec.report_open_position_at_end = true;
        require(configure_native(spec).status == pineforge::NativeSetupStatus::Applied,
                "native fixture configure refused");
    }
    pf_strategy_t handle() { return static_cast<BacktestEngine*>(this); }
    std::array<std::size_t, 4> report_storage() const {
        return {equity_curve_.size(), equity_curve_.capacity(),
                trades_.base(), trades_.resident_base()};
    }
    pineforge::metrics::ScratchCurveRegion scratch_region() const {
        return report_sink_equity_scratch_region();
    }
    bool abort = false;
    bool fail = false;
    int callbacks = 0;
    int busy_fd = 99, busy_path = 99;

    void on_native_run_begin() override {
        callbacks = 0;
        output_run_begin();
        output_constant(0, -0.0);
        output_constant(1, std::numeric_limits<double>::infinity());
    }
    void on_native_bar(const Bar& bar, const pineforge::NativeDecisionContext& context) override {
        const int index = callbacks++;
        if (index == 0) {
            busy_fd = strategy_set_report_sink_fd_v1(handle(), nullptr);
            busy_path = strategy_set_report_sink_v1(handle(), nullptr);
        }
        output_bar(context.script_bar_open_ms, context.script_bar_open_ms + 60000);
        output_value(0, bar.close);
        output_value(1, index % 2 ? -0.0 : std::numeric_limits<double>::quiet_NaN());
        output_event(0, bar.close, std::string(16384, 'x') + "\"\\\n\xc3\xa9");
        trace("quote\"\\\n", bar.close);
        if (index == 0 || index == 8 || index == 16) {
            const auto placed = submit({pineforge::native_order::Transact{index == 8 ? -1.0 : 1.0},
                                        index == 8 ? "exit" : "entry", ""});
            require(placed.status == pineforge::native_order::SubmitStatus::Accepted,
                    "fixture transaction refused");
        }
        if (index == 8 && abort) request_abort();
        if (index == 8 && fail) throw std::runtime_error("deliberate fixture failure");
    }
};

pf_report_sink_receipt_v1 receipt(Fixture& host) {
    pf_report_sink_receipt_v1 result{};
    result.struct_size = sizeof result;
    result.version = 1;
    require(strategy_report_sink_receipt_v1(host.handle(), &result) == 0, "receipt query refused");
    return result;
}

pf_report_sink_summary_v1 summary(Fixture& host) {
    pf_report_sink_summary_v1 result{};
    result.struct_size = sizeof result;
    result.version = 1;
    require(strategy_report_sink_summary_v1(host.handle(), &result) == 0, "summary query refused");
    return result;
}

void install_fd(Fixture& host, int fd) {
    std::string copied_id = run_id;
    const pf_report_sink_fd_v1 descriptor{sizeof descriptor, 1, fd, copied_id.c_str()};
    require(strategy_set_report_sink_fd_v1(host.handle(), &descriptor) == 0, "fd sink refused");
    copied_id.assign(copied_id.size(), '?'); // setter must have copied it
}

void install_path(Fixture& host, const fs::path& path) {
    const auto text = path.string();
    const pf_report_sink_v1 descriptor{sizeof descriptor, 1, text.c_str(), run_id, nullptr};
    require(strategy_set_report_sink_v1(host.handle(), &descriptor) == 0, "path sink refused");
}

struct Result {
    pf_report_sink_receipt_v1 receipt{};
    pf_metrics_t metrics{};
    int total_trades = 0;
    std::uint64_t hash = 0;
};
enum class Expect { success, sink_failure, incomplete };

std::string run_diagnostics(Fixture& host, const std::vector<Bar>& bars) {
    return "status=" + std::to_string(host.last_run_status())
        + ", code=" + strategy_get_last_error_code(host.handle())
        + ", args=" + strategy_get_last_error_args(host.handle())
        + ", callbacks=" + std::to_string(host.callbacks) + "/" + std::to_string(bars.size())
        + ", last_error=" + host.last_error();
}

Result execute(Fixture& host, const std::vector<Bar>& bars, Expect expect = Expect::success) {
    struct Report {
        pineforge::ReportC value{};
        ~Report() { BacktestEngine::free_report(&value); }
    } report;
    try {
        host.run(bars.data(), static_cast<int>(bars.size()));
        host.fill_report(&report.value);
    } catch (const std::exception& error) {
        if (expect == Expect::success) {
            std::fprintf(stderr, "L22A engine exception: %s\n", run_diagnostics(host, bars).c_str());
            throw;
        }
        // Same boundary as generated run_backtest_full: preserve carried
        // codes from a real run/capture failure instead of inventing a result.
        pineforge::note_run_failure(host, "L22A engine run", error);
    }
    Result result{receipt(host), report.value.metrics, report.value.total_trades,
                  host.broker_state_hash()};
    if (expect == Expect::success) {
        require(host.last_run_status() == 0 && host.last_error().empty()
                && host.callbacks == static_cast<int>(bars.size()),
                "engine run failed or stopped early: " + run_diagnostics(host, bars));
        require(result.receipt.complete == 1 && result.receipt.count_e > 0
                && result.receipt.count_t > 0, "sink did not complete nonempty E/T records");
        require(report.value.equity_curve == nullptr && report.value.trades == nullptr,
                "sink did not return a summary report");
        require(host.busy_fd == -3 && host.busy_path == -3, "setter accepted an active batch run");
        // A second capture must not append another trailer or change receipt.
        BacktestEngine::free_report(&report.value);
        report.value = {};
        host.fill_report(&report.value);
        const auto again = receipt(host);
        require(std::memcmp(&again, &result.receipt, sizeof again) == 0,
                "repeat capture changed receipt");
    } else {
        require(host.last_run_status() != 0 || !host.last_error().empty(),
                "failed/aborted run reported success");
        require(result.receipt.complete == 0 && result.receipt.sha256_hex[0] == '\0',
                "failed/aborted run completed the sink");
        if (expect == Expect::sink_failure)
            require(std::strcmp(strategy_get_last_error_code(host.handle()), "report_sink_failed") == 0,
                    "stream failure lost typed report_sink_failed");
    }
    return result;
}

void equal_result(const Result& got, const Result& expected) {
    require(std::memcmp(&got.receipt, &expected.receipt, sizeof got.receipt) == 0,
            "fd/path receipts differ");
    require(std::memcmp(&got.metrics, &expected.metrics, sizeof got.metrics) == 0
            && got.total_trades == expected.total_trades && got.hash == expected.hash,
            "fd/path metrics, trades or broker hash differ");
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "cannot read test sink");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void save(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(bool(output), "cannot save captured stream for the strict reader");
}

Result baseline(const fs::path& directory, const std::vector<Bar>& bars) {
    auto unused = stream(Transport::pipe, directory);
    Result result;
    {
        Fixture host;
        install_fd(host, unused->fd());
        install_path(host, directory / "path.ndjson"); // path replaces fd
        result = execute(host, bars);
        require(strategy_set_report_sink_fd_v1(host.handle(), nullptr) == -3,
                "fd setter accepted removal of a scratch-backed path generation");
        require(receipt(host).complete == 1, "refused sink removal discarded the completed receipt");
    }
    require(::fcntl(unused->fd(), F_GETFD) >= 0, "path replacement closed borrowed fd");
    require(unused->finish().empty(), "replaced fd sink received bytes");
    return result;
}

void unchanged_generation(Fixture& host, const Result& before,
                           const pf_report_sink_summary_v1& before_summary) {
    const auto after_receipt = receipt(host);
    const auto after_summary = summary(host);
    require(std::memcmp(&after_receipt, &before.receipt, sizeof after_receipt) == 0,
            "NULL removal changed the completed receipt");
    require(std::memcmp(&after_summary, &before_summary, sizeof after_summary) == 0,
            "NULL removal changed the completed summary");
    struct Report {
        pineforge::ReportC value{};
        ~Report() { BacktestEngine::free_report(&value); }
    } report;
    host.fill_report(&report.value);
    require(report.value.trades == nullptr && report.value.trades_len == 0
            && report.value.equity_curve == nullptr && report.value.equity_curve_len == 0
            && report.value.broker_state_hash == nullptr && report.value.broker_state_hash_len == 0
            && report.value.trace == nullptr && report.value.trace_len == 0,
            "NULL removal changed the completed summary report into arrays");
    require(std::memcmp(&report.value.metrics, &before.metrics, sizeof before.metrics) == 0
            && report.value.total_trades == before.total_trades,
            "NULL removal changed the completed report metrics or logical trades");
    require(host.last_run_status() == 0 && host.last_error().empty()
            && host.broker_state_hash() == before.hash,
            "NULL removal changed the run outcome or broker hash");
}

void unchanged_storage(Fixture& host, const std::array<std::size_t, 4>& before,
                       const pineforge::metrics::ScratchCurveRegion& scratch) {
    require(host.report_storage() == before, "NULL removal rehydrated report history");
    const auto after = host.scratch_region();
    require(after.scratch == scratch.scratch && after.descriptor == scratch.descriptor
            && after.byte_offset == scratch.byte_offset && after.point_count == scratch.point_count,
            "NULL removal dropped or changed the scratch owner");
}

void completed_fd_removal(const fs::path& directory, const std::vector<Bar>& bars,
                          const std::string& bytes, bool through_path) {
    auto output = stream(Transport::pipe, directory);
    const int flags = ::fcntl(output->fd(), F_GETFL);
    const int descriptor_flags = ::fcntl(output->fd(), F_GETFD);
    require(flags >= 0 && descriptor_flags >= 0, "cannot read removal fixture fd flags");
    {
        Fixture host;
        install_fd(host, output->fd());
        const auto before = execute(host, bars);
        const auto before_summary = summary(host);
        const auto storage = host.report_storage();
        const auto scratch = host.scratch_region();
        require(before_summary.complete == 1 && before_summary.count_t == before.receipt.count_t,
                "fd removal fixture has no completed T-row summary");
        require(scratch.point_count > 0 && (scratch.scratch || scratch.descriptor >= 0)
                && storage[0] < static_cast<std::size_t>(before.receipt.count_e),
                "fd removal fixture has no live scratch-backed curve");
        const auto remove = [&] {
            return through_path ? strategy_set_report_sink_v1(host.handle(), nullptr)
                                : strategy_set_report_sink_fd_v1(host.handle(), nullptr);
        };
        require(remove() == 0, through_path ? "path NULL refused a completed fd generation"
                                           : "fd NULL refused a completed fd generation");
        unchanged_storage(host, storage, scratch);
        unchanged_generation(host, before, before_summary);
        unchanged_storage(host, storage, scratch);
        require(remove() == 0, "repeated NULL removal refused a completed fd generation");
        unchanged_generation(host, before, before_summary);
        unchanged_storage(host, storage, scratch);
        require(::fcntl(output->fd(), F_GETFL) == flags
                && ::fcntl(output->fd(), F_GETFD) == descriptor_flags,
                "NULL removal closed the borrowed fd or changed its flags");

        // A retained receipt must not keep the sink installed for the next
        // run: that run owns ordinary arrays and writes nothing to this fd.
        // A Completed native host needs a fresh Ready spec before another begin.
        host.configure_run(2);
        host.run(bars.data(), static_cast<int>(bars.size()));
        struct Report {
            pineforge::ReportC value{};
            ~Report() { BacktestEngine::free_report(&value); }
        } next;
        host.fill_report(&next.value);
        require(host.last_run_status() == 0 && host.last_error().empty()
                && host.callbacks == static_cast<int>(bars.size()),
                "sink-off run after NULL removal failed");
        require(next.value.trades != nullptr && next.value.trades_len == before.total_trades
                && next.value.equity_curve != nullptr
                && next.value.equity_curve_len == before.receipt.count_e,
                "NULL removal did not detach the sink for the next run");
        require(receipt(host).complete == 0 && summary(host).complete == 0,
                "next run retained the previous generation's completion");
    }
    require(::fcntl(output->fd(), F_GETFD) == descriptor_flags,
            "destruction after NULL removal closed the borrowed fd");
    require(output->finish() == bytes, "NULL removal or the next run appended sink bytes");
}

void ordinary_scratch_removal_refused(const fs::path& directory, const std::vector<Bar>& bars) {
    Fixture host;
    const auto path = directory / "ordinary-removal.ndjson";
    install_path(host, path);
    const auto before = execute(host, bars);
    const auto before_summary = summary(host);
    const auto storage = host.report_storage();
    const auto scratch = host.scratch_region();
    const auto bytes = read_file(path);
    require(scratch.point_count > 0 && (scratch.scratch || scratch.descriptor >= 0),
            "ordinary removal control has no live scratch owner");
    require(strategy_set_report_sink_v1(host.handle(), nullptr) == -3,
            "ordinary scratch removal lost its -3 refusal");
    unchanged_storage(host, storage, scratch);
    unchanged_generation(host, before, before_summary);
    unchanged_storage(host, storage, scratch);
    require(read_file(path) == bytes, "ordinary refusal or repeat capture appended sink bytes");
}

std::string fd_run(const fs::path& directory, const std::vector<Bar>& bars,
                   const Result& expected, Transport transport, bool slow = false) {
    auto output = stream(transport, directory, slow);
    const int flags = ::fcntl(output->fd(), F_GETFL);
    const int descriptor_flags = ::fcntl(output->fd(), F_GETFD);
    require(flags >= 0 && descriptor_flags >= 0, "cannot read caller fd flags");
    {
        Fixture host;
        const auto replaced = directory / "must-not-exist.ndjson";
        install_path(host, replaced);
        install_fd(host, output->fd()); // fd replaces path, no path parent used
        equal_result(execute(host, bars), expected);
        require(!fs::exists(replaced), "fd sink opened the replaced path");
        pf_report_sink_fd_v1 invalid{sizeof invalid, 2, output->fd(), run_id};
        require(strategy_set_report_sink_fd_v1(host.handle(), &invalid) == -1,
                "bad replacement version accepted");
        require(receipt(host).complete == 1, "bad replacement erased completed receipt");
        require(::fcntl(output->fd(), F_GETFL) == flags
                && ::fcntl(output->fd(), F_GETFD) == descriptor_flags,
                "writer changed caller fd flags");
        require(strategy_set_report_sink_v1(host.handle(), nullptr) == 0,
                "path setter cannot remove an fd sink");
        require(::fcntl(output->fd(), F_GETFD) >= 0, "sink removal closed caller fd");
        install_fd(host, output->fd()); // destruction must not close it either
    }
    require(::fcntl(output->fd(), F_GETFD) >= 0, "handle destruction closed caller fd");
    return output->finish();
}

std::vector<std::string> entries(const fs::path& directory) {
    std::vector<std::string> result;
    for (const auto& entry : fs::recursive_directory_iterator(directory))
        result.push_back(entry.path().lexically_relative(directory).string());
    std::sort(result.begin(), result.end());
    return result;
}

class JailScope {
public:
    explicit JailScope(const fs::path& directory) : old_directory_(fs::current_path()) {
        if (const auto* value = std::getenv("TMPDIR")) { had_tmp_ = true; old_tmp_ = value; }
        require(::getrlimit(RLIMIT_FSIZE, &previous_) == 0, "getrlimit failed");
        require(::setenv("TMPDIR", directory.c_str(), 1) == 0, "cannot set jail TMPDIR");
        if (::chdir(directory.c_str()) != 0) {
            restore();
            throw std::runtime_error("cannot enter jail directory");
        }
        auto limit = previous_;
        limit.rlim_cur = 0;
        if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            restore();
            throw std::runtime_error("cannot apply RLIMIT_FSIZE=0");
        }
    }
    ~JailScope() { restore(); }
    JailScope(const JailScope&) = delete;
    JailScope& operator=(const JailScope&) = delete;
private:
    void restore() noexcept {
        ::setrlimit(RLIMIT_FSIZE, &previous_);
        ::chdir(old_directory_.c_str());
        if (had_tmp_) ::setenv("TMPDIR", old_tmp_.c_str(), 1);
        else ::unsetenv("TMPDIR");
    }
    fs::path old_directory_;
    struct rlimit previous_{};
    bool had_tmp_ = false;
    std::string old_tmp_;
};

class ArenaScope {
public:
    explicit ArenaScope(const fs::path& directory)
        : backing_(::open((directory / "inherited-arena").c_str(),
                          O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600)) {
        require(backing_.get() >= 0, "cannot prepare inherited scratch arena");
        require(::unlink((directory / "inherited-arena").c_str()) == 0,
                "cannot unlink inherited scratch arena");
        // Host capacity, not an RSS bound: the command backing's 1/16 share
        // (src/command_scratch_production.hpp) gives each store one chunk at
        // A = 16*C. Reserving all three shares leaves 13 chunks for this
        // three-transaction fixture's report histories; its 32-point equity
        // curve is just 32*sizeof(pf_equity_point_t) = 768 bytes.
        constexpr std::uint64_t bytes = 16 * pineforge::detail::ScratchArena::kChunkBytes;
        require(::ftruncate(backing_.get(), static_cast<off_t>(bytes)) == 0,
                "cannot size inherited scratch arena before the jail");
        pineforge::detail::ScratchArena::attach(backing_.get(), bytes);
    }
    ~ArenaScope() { pineforge::detail::ScratchArena::detach(); }
    ArenaScope(const ArenaScope&) = delete;
    ArenaScope& operator=(const ArenaScope&) = delete;
private:
    Descriptor backing_;
};

void jailed(const fs::path& directory, const std::vector<Bar>& bars,
            const Result& expected, const std::string& bytes) {
    const auto jail = directory / "jail";
    fs::create_directory(jail);
    std::string captured;
    {
        SignalScope size_signal(SIGXFSZ, SIG_IGN);
        // The fd supplies output only; provision scratch before the jail.
        // Mutation: in include/pineforge/detail/scratch_file.hpp, open(),
        // select `if (auto* arena = ScratchArena::attached(); false)`.
        // The jailed fd run must fail when its first trade row needs scratch.
        ArenaScope arena(directory);
        JailScope limits(jail); // applies BEFORE constructing/running the engine
        struct rlimit current{};
        require(::getrlimit(RLIMIT_FSIZE, &current) == 0 && current.rlim_cur == 0,
                "test did not enter file-size jail");
        const auto before = entries(jail);
        captured = fd_run(jail, bars, expected, Transport::pipe);
        const auto after = entries(jail);
        require(before == after && after.empty(), "fd run created a regular/scratch/temp file");
        require(captured == bytes, "jailed fd stream differs from path sink");
        Fixture rejected;
        const auto path = jail / "path-rejected.ndjson";
        install_path(rejected, path);
        execute(rejected, bars, Expect::sink_failure);
        require(fs::is_regular_file(path) && fs::file_size(path) == 0,
                "path sink wrote bytes inside RLIMIT_FSIZE=0");
        require_signal(SIGXFSZ, SIG_IGN);
        require_signal(SIGPIPE, SIG_IGN);
    }
    save(directory / "jailed.ndjson", captured);
}

void incomplete(const fs::path& directory, const std::vector<Bar>& bars,
                Transport transport, bool peer_closed, bool abort) {
    auto output = stream(transport, directory, false, peer_closed ? 8192 : 0);
    {
        Fixture host;
        host.abort = abort;
        host.fail = !peer_closed && !abort;
        install_fd(host, output->fd());
        execute(host, bars, peer_closed ? Expect::sink_failure : Expect::incomplete);
        require(host.callbacks < static_cast<int>(bars.size()), "failure did not interrupt the run");
        require(::fcntl(output->fd(), F_GETFD) >= 0, "failure closed borrowed fd");
    }
    require(::fcntl(output->fd(), F_GETFD) >= 0, "failed handle destruction closed borrowed fd");
    const auto bytes = output->finish();
    require(!bytes.empty() && bytes.find("{\"trailer\":") == std::string::npos,
            "failed run emitted a trailer or fixture never reached the stream");
    if (peer_closed) require(bytes.size() == 8192, "reader did not close after N bytes");
    const auto name = peer_closed
        ? transport == Transport::socket ? "socket-closed" : "pipe-closed"
        : abort ? "aborted" : "callback-failed";
    save(directory / (std::string(name) + ".ndjson"), bytes);
}

void refusals(const fs::path& directory, const std::vector<Bar>& bars) {
    Fixture host;
    auto output = stream(Transport::pipe, directory);
    require(strategy_set_report_sink_fd_v1(nullptr, nullptr) == -1, "null handle accepted");
    pf_report_sink_fd_v1 descriptor{sizeof descriptor, 1, -1, nullptr};
    require(strategy_set_report_sink_fd_v1(host.handle(), &descriptor) == -1, "negative fd accepted");
    descriptor.fd = output->fd();
    descriptor.version = 2;
    require(strategy_set_report_sink_fd_v1(host.handle(), &descriptor) == -1, "bad version accepted");
    descriptor.version = 1;
    --descriptor.struct_size;
    require(strategy_set_report_sink_fd_v1(host.handle(), &descriptor) == -1, "bad size accepted");
    descriptor.struct_size = sizeof descriptor;
    require(strategy_set_report_sink_fd_v1(host.handle(), &descriptor) == 0, "NULL run_id refused");
    require(strategy_set_report_sink_fd_v1(host.handle(), nullptr) == 0, "fd removal refused");
    // No sink during the stream; both install and remove must refuse it.
    const auto& bar = bars.front();
    pf_bar_t input{bar.open, bar.high, bar.low, bar.close, bar.volume, bar.timestamp};
    require(strategy_stream_begin(host.handle(), &input, 1, "1", "1") == 0, "stream begin failed");
    require(strategy_set_report_sink_fd_v1(host.handle(), &descriptor) == -2
            && strategy_set_report_sink_fd_v1(host.handle(), nullptr) == -2
            && strategy_set_report_sink_v1(host.handle(), nullptr) == -2,
            "setter accepted an open stream");
    require(strategy_stream_end(host.handle(), 0) == 0, "stream end failed");
    require(output->finish().empty(), "removed sink received stream bytes");
    // A regular file must not become the fd route's output or be truncated.
    const auto regular_path = directory / "regular-input";
    save(regular_path, "caller-owned");
    Descriptor regular(::open(regular_path.c_str(), O_WRONLY));
    require(regular.get() >= 0, "cannot open regular descriptor fixture");
    {
        Fixture invalid;
        install_fd(invalid, regular.get());
        execute(invalid, bars, Expect::sink_failure);
    }
    require(::fcntl(regular.get(), F_GETFD) >= 0 && read_file(regular_path) == "caller-owned",
            "invalid fd route modified or closed a regular file");
}

void strict_reader(const fs::path& directory) {
    const auto child = ::fork();
    require(child >= 0, "reader fork failed");
    if (child == 0) {
        ::execl(PINEFORGE_L22A_PYTHON, PINEFORGE_L22A_PYTHON, "-B",
                PINEFORGE_L22A_READER, directory.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "existing strict record parser rejected the captured streams");
}
} // namespace

int main() {
    try {
        SignalScope pipe_signal(SIGPIPE, SIG_IGN); // host's FIFO contract
        char text[] = "/tmp/pineforge-l22a-XXXXXX";
        require(::mkdtemp(text) != nullptr, "cannot create test directory");
        const fs::path directory(text);
        const auto bars = feed();
        const auto expected = baseline(directory, bars);
        const auto bytes = read_file(directory / "path.ndjson");
        completed_fd_removal(directory, bars, bytes, true);
        completed_fd_removal(directory, bars, bytes, false);
        ordinary_scratch_removal_refused(directory, bars);
        for (const auto transport : {Transport::pipe, Transport::fifo, Transport::socket}) {
            const auto captured = fd_run(directory, bars, expected, transport, transport == Transport::fifo);
            require(captured == bytes, "fd stream bytes differ from the real path-sink run");
            const auto name = transport == Transport::pipe ? "pipe" : transport == Transport::fifo ? "fifo" : "socket";
            save(directory / (std::string(name) + ".ndjson"), captured);
            require_signal(SIGPIPE, SIG_IGN);
        }
        jailed(directory, bars, expected, bytes);
        incomplete(directory, bars, Transport::pipe, true, false);
        {
            SignalScope default_pipe(SIGPIPE, SIG_DFL);
            incomplete(directory, bars, Transport::socket, true, false);
            require_signal(SIGPIPE, SIG_DFL); // send(MSG_NOSIGNAL) must suffice
        }
        incomplete(directory, bars, Transport::pipe, false, true);
        incomplete(directory, bars, Transport::pipe, false, false);
        refusals(directory, bars);
        strict_reader(directory);
        fs::remove_all(directory);
        std::puts("L22A fd/path identity, RLIMIT_FSIZE jail, back-pressure, socket and failure lifecycle: passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "L22A: %s\n", error.what());
        return 1;
    }
}
