// Equity-curve scratch files follow ordinary and selected report lifetimes.
// Include the production implementation to complete ScratchCurveWriter::State,
// through direct implementation inclusion. The archive supplies the
// engine/consumer; no product accessor, layout copy or alternate writer exists.
#include "../src/report_scratch.cpp"
#include "../src/native_execution_consumer.hpp"

#include <pineforge/selected_window.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v19 {
int native_set_selected_window_v1(
    BacktestEngine*, const pf_selected_window_config_v1*) noexcept;
}
}

namespace memory_spill_equity_curve {
using namespace pineforge;
namespace rs = pineforge::report_sink;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Sink { friend auto access(Sink); };
struct Presentation { friend auto access(Presentation); };
struct WriterState { friend auto access(WriterState); };
template struct Access<Sink, &BacktestEngine::report_sink_state_>;
template struct Access<Presentation, &NativeExecutionConsumer::presentation_metric_curve_>;
template struct Access<WriterState, &rs::ScratchCurveWriter::state_>;

std::size_t checks = 0, failures = 0;
std::string trial_name;
struct PrerequisiteFailure {};

bool check(bool passed, const char* expression, int line) {
    ++checks;
    if (!passed) {
        ++failures;
        std::fprintf(stderr, "FAIL [equity-curve] %s:%d: %s (trial=%s)\n",
                     __FILE__, line, expression, trial_name.c_str());
    }
    return passed;
}
#define MEM_CHECK(expression) check(bool(expression), #expression, __LINE__)
#define MEM_NEED(expression) do { \
    if (!MEM_CHECK(expression)) throw PrerequisiteFailure{}; \
} while (false)

template<class Action>
void trial(const std::string& name, Action action) {
    trial_name = name;
    try { action(); }
    catch (const PrerequisiteFailure&) {} // Already counted; run the next trial.
    catch (const std::exception& error) { check(false, error.what(), __LINE__); }
    catch (...) { check(false, "unexpected exception", __LINE__); }
}

// report_sink::LineWriter's fd sink accepts only FIFOs or stream sockets,
// so a tmpfile fd fails at begin.
// Drain concurrently: capture/fill_report can publish more than the socket
// buffer holds. Keep only a fixed reader buffer.
class Stream {
public:
    Stream() {
        MEM_NEED(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors_) == 0);
        try {
            reader_ = std::thread([this] {
                std::array<char, 4096> buffer{};
                for (;;) {
                    const auto count = ::read(descriptors_[0], buffer.data(), buffer.size());
                    if (count < 0 && errno == EINTR) continue;
                    if (count <= 0) {
                        if (count < 0) {
                            read_error_ = errno;
                            // Let a blocked writer observe a failed reader.
                            ::shutdown(descriptors_[0], SHUT_RD);
                        }
                        return;
                    }
                }
            });
        } catch (...) {
            ::close(descriptors_[0]);
            ::close(descriptors_[1]);
            throw;
        }
    }
    ~Stream() {
        // The host is declared after this stream and is already destroyed.
        ::close(descriptors_[1]);
        reader_.join();
        ::close(descriptors_[0]);
        MEM_CHECK(read_error_ == 0);
    }
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    int fd() const { return descriptors_[1]; }
private:
    int descriptors_[2]{-1, -1};
    int read_error_ = 0; // Read by the main thread only after join().
    std::thread reader_;
};

constexpr std::int64_t start = 1736121600000LL;
constexpr std::int64_t minute = 60000;
constexpr double capital = 1000000.0;
// Derived from the array initializer and its byte budget in report_scratch.cpp
// and report_scratch.hpp, independently of the production capacity constant.
constexpr std::size_t batch_bytes = 64U * 1024U;
constexpr std::size_t point_bytes = sizeof(pf_equity_point_t);
constexpr std::size_t batch_points = batch_bytes / point_bytes;

auto& state(rs::ScratchCurveWriter& writer) {
    auto& owner = writer.*access(WriterState{});
    MEM_NEED(owner);
    return *owner;
}

std::uint64_t file_bytes(rs::ScratchCurveWriter& writer) {
    // Observe the backing without region()/flush(): the stimulus itself must
    // have written a batch before the explicit final read below.
    const int fd = state(writer).file.descriptor();
    MEM_NEED(fd >= 0); // These trials intentionally use the ordinary file backend.
    struct stat info{};
    MEM_NEED(::fstat(fd, &info) == 0);
    MEM_NEED(info.st_size >= 0);
    return static_cast<std::uint64_t>(info.st_size);
}

void snapshot(rs::ScratchCurveWriter& writer, std::size_t expected_points) {
    const auto& actual = state(writer);
    MEM_CHECK(writer.size() == static_cast<std::int64_t>(expected_points));
    // The pending array's extent is fixed by type.
    MEM_CHECK(actual.pending.size() == batch_points);
    MEM_CHECK(actual.pending.size() * point_bytes <= batch_bytes);
    MEM_CHECK(actual.pending_count <= batch_points);
    MEM_CHECK(actual.disk_points >= 0);
    MEM_CHECK(actual.disk_points + static_cast<std::int64_t>(actual.pending_count)
             == static_cast<std::int64_t>(expected_points));
    const auto bytes = file_bytes(writer);
    MEM_CHECK(bytes == static_cast<std::uint64_t>(actual.disk_points) * point_bytes);
    // Some Debug report checks flush early. Conservation plus the independent
    // RAM budget gives this bound without assuming their exact flush cadence.
    const auto minimum_disk_points =
        expected_points > batch_points ? expected_points - batch_points : 0;
    MEM_CHECK(bytes >= minimum_disk_points * point_bytes);
    MEM_CHECK(bytes <= expected_points * point_bytes);
}

std::uint64_t curve_equal(rs::ScratchCurveWriter& writer,
                          const std::vector<pf_equity_point_t>& expected) {
    snapshot(writer, expected.size());
    const auto region = writer.region();
    MEM_CHECK(region.point_count == static_cast<std::int64_t>(expected.size()));
    MEM_NEED(region.scratch);
    MEM_CHECK(region.scratch->disk_backed());
    const auto bytes = file_bytes(writer);
    MEM_CHECK(bytes == expected.size() * point_bytes);
    MEM_CHECK(state(writer).disk_points == static_cast<std::int64_t>(expected.size()));
    MEM_CHECK(state(writer).pending_count == 0);
    // Compare every stored semantic field, not just the first/last point or a
    // count returned by a report. The oracle is built only from the input tape.
    MEM_NEED(region.point_count >= 0);
    const auto common = std::min(expected.size(), static_cast<std::size_t>(region.point_count));
    bool whole_curve_equal = region.point_count == static_cast<std::int64_t>(expected.size());
    std::array<pf_equity_point_t, 127> buffer{};
    for (std::size_t first = 0; first < common; first += buffer.size()) {
        const auto count = std::min(buffer.size(), common - first);
        rs::read_scratch_points(region, static_cast<std::int64_t>(first),
                                buffer.data(), static_cast<std::int64_t>(count));
        for (std::size_t i = 0; i < count; ++i) {
            const auto& want = expected[first + i];
            const auto& got = buffer[i];
            whole_curve_equal = whole_curve_equal
                && got.time_ms == want.time_ms && got.equity == want.equity
                && got.open_profit == want.open_profit;
        }
    }
    MEM_CHECK(whole_curve_equal);
    return bytes;
}

enum class Mode { Ordinary, SelectedHost, SelectedKernel };

class Host final : public NativeStrategyHost {
public:
    Mode mode = Mode::Ordinary;
    std::size_t callbacks = 0, samples = 0;
    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }
    auto& recorded() { return (this->*access(Sink{})).equity_scratch; }
    auto& presented() { return consumer().*access(Presentation{}); }

    void on_native_run_begin() override {
        callbacks = samples = 0;
        // This is before capture can overwrite an old owner with a new one.
        MEM_CHECK(!presented());
        if (mode == Mode::SelectedHost) {
            // Unlike ordinary begin, this branch does not create a replacement
            // writer, so it exposes the actual sink reset, not reassignment.
            MEM_CHECK(!recorded());
        } else {
            MEM_NEED(recorded());
            snapshot(*recorded(), mode == Mode::SelectedKernel ? 1 : 0);
        }
    }

    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        ++callbacks;
        if (mode == Mode::SelectedHost) {
            // The real HostRecorded producer owns these observations. Capture
            // must move them through its production tail/append/flush path.
            // The target scratch writer is never populated by test code.
            equity_curve_.push_back({bar.timestamp, capital + bar.close - 100.0, 0.0});
            return;
        }
        if (callbacks == 1 || callbacks == batch_points + 3
            || callbacks == 2 * batch_points + 3) {
            MEM_NEED(recorded());
            // KernelRecorded records AFTER this callback. Of the preceding
            // callbacks, one point is held and all older ones are in scratch.
            // Selection additionally retains its capital anchor.
            const auto released = callbacks > 1 ? callbacks - 2 : 0;
            const auto expected = released + (mode == Mode::SelectedKernel ? 1 : 0);
            snapshot(*recorded(), expected);
            if (expected > batch_points) MEM_CHECK(file_bytes(*recorded()) > 0);
            ++samples;
        }
    }
};

struct Report {
    ReportC value{};
    ~Report() { BacktestEngine::free_report(&value); }
};

NativeRunSpec spec(Mode mode, std::uint64_t generation) {
    NativeRunSpec result;
    result.identity = {"equity-curve", generation};
    result.input_tf = result.script_tf = "1";
    result.ticker = "MOCK"; result.tickerid = "TEST:CURVE";
    result.type = "crypto"; result.currency = "USD";
    result.timezone = "UTC"; result.session = "24x7";
    result.initial_capital = capital;
    result.point_value = result.account_fx = 1.0;
    result.price_tick = 0.01;
    result.report_policy = mode == Mode::SelectedHost
        ? NativeReportPolicy::HostRecorded : NativeReportPolicy::KernelRecorded;
    return result;
}

std::vector<Bar> tape(std::size_t count) {
    std::vector<Bar> bars;
    bars.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double price = 100.0 + static_cast<double>(i % 31) / 4.0;
        bars.push_back({price, price, price, price, 1.0,
                       start + static_cast<std::int64_t>(i) * minute});
    }
    return bars;
}

std::vector<pf_equity_point_t> model(const std::vector<Bar>& bars, Mode mode) {
    std::vector<pf_equity_point_t> expected;
    if (mode != Mode::Ordinary) expected.push_back({start, capital, 0.0});
    for (const auto& bar : bars) {
        // No order is submitted: the kernel account stays at initial capital.
        // A HostRecorded host instead reports the specified input-price series.
        const double equity = mode == Mode::SelectedHost
            ? capital + (bar.close - 100.0) : capital;
        expected.push_back({bar.timestamp, equity, 0.0});
    }
    return expected;
}

void install(Host& host, const Stream& stream) {
    MEM_NEED(stream.fd() >= 0);
    const pf_report_sink_fd_v1 sink{sizeof sink, 1, stream.fd(), "equity-curve"};
    MEM_NEED(host.set_report_sink_fd_v1(&sink) == 0);
}

void run(Host& host, Mode mode, std::uint64_t generation, const std::vector<Bar>& bars) {
    host.mode = mode;
    MEM_NEED(host.configure_native(spec(mode, generation)).status == NativeSetupStatus::Applied);
    const pf_selected_window_config_v1 window{
        sizeof window, 1, start, start + static_cast<std::int64_t>(bars.size()) * minute};
    MEM_NEED(native_set_selected_window_v1(
        &host, mode == Mode::Ordinary ? nullptr : &window) == 0);
    host.run(bars.data(), static_cast<int>(bars.size()));
    // NativeStrategyHost::run carries a native failure's diagnostic in
    // last_error(), before completion.
    if (!host.last_error().empty())
        std::fprintf(stderr, "equity-curve native run error (trial=%s): %s\n",
                     trial_name.c_str(), host.last_error().c_str());
    MEM_NEED(host.last_error().empty());
    MEM_NEED(host.native_state().kind == NativeLifecycleKind::Completed);
    MEM_CHECK(host.callbacks == bars.size());
    if (mode != Mode::SelectedHost) {
        const std::size_t expected_samples = 1
            + (bars.size() >= batch_points + 3 ? 1 : 0)
            + (bars.size() >= 2 * batch_points + 3 ? 1 : 0);
        MEM_CHECK(host.samples == expected_samples);
    }
}

std::uint64_t read_twice(Host& host, rs::ScratchCurveWriter& writer,
                         const std::vector<pf_equity_point_t>& expected) {
    const auto bytes = curve_equal(writer, expected);
    Report repeat;
    host.fill_report(&repeat.value);
    MEM_CHECK(writer.size() == static_cast<std::int64_t>(expected.size()));
    MEM_CHECK(file_bytes(writer) == bytes);
    MEM_CHECK(curve_equal(writer, expected) == bytes);
    return bytes;
}

struct Counts {
    std::int64_t ordinary_points = 0, presentation_points = 0;
    std::uint64_t ordinary_bytes = 0, presentation_bytes = 0;
    std::size_t ordinary_slots = 0, presentation_slots = 0;
};

Counts ordinary_then_host_recorded(std::size_t n) {
    const auto bars = tape(n);
    Stream stream;
    Host host;
    install(host, stream);
    run(host, Mode::Ordinary, 1, bars);
    Report ordinary;
    host.fill_report(&ordinary.value); // Stages and flushes the final held point.
    MEM_NEED(host.recorded());
    MEM_CHECK(!host.presented());
    Counts result;
    result.ordinary_bytes = read_twice(host, *host.recorded(), model(bars, Mode::Ordinary));
    result.ordinary_points = host.recorded()->size();
    result.ordinary_slots = state(*host.recorded()).pending.size();

    // begin_report_sink must release the ordinary file before this callback.
    // HostRecorded selection skips creation of a replacement engine writer.
    run(host, Mode::SelectedHost, 2, bars);
    MEM_CHECK(!host.recorded());
    MEM_NEED(host.presented());
    result.presentation_bytes =
        read_twice(host, *host.presented(), model(bars, Mode::SelectedHost));
    result.presentation_points = host.presented()->size();
    result.presentation_slots = state(*host.presented()).pending.size();

    // A real next begin must release the selected presentation before its
    // callback; a two-bar run must retain only its own two observations.
    run(host, Mode::Ordinary, 3, tape(2));
    MEM_CHECK(!host.presented());
    Report short_report;
    host.fill_report(&short_report.value);
    MEM_NEED(host.recorded());
    curve_equal(*host.recorded(), model(tape(2), Mode::Ordinary));
    return result;
}

Counts selected_kernel(std::size_t n) {
    const auto bars = tape(n);
    Stream stream;
    Host host;
    install(host, stream);
    run(host, Mode::SelectedKernel, 1, bars);
    // The selected kernel route transfers the record-time writer at capture.
    MEM_CHECK(!host.recorded());
    MEM_NEED(host.presented());
    Counts result;
    result.presentation_bytes =
        read_twice(host, *host.presented(), model(bars, Mode::SelectedKernel));
    result.presentation_points = host.presented()->size();
    result.presentation_slots = state(*host.presented()).pending.size();
    run(host, Mode::Ordinary, 2, tape(2));
    MEM_CHECK(!host.presented());
    return result;
}
} // namespace memory_spill_equity_curve

int main() {
    using namespace memory_spill_equity_curve;
    std::array<std::optional<Counts>, 2> ordinary{}, selected{};
    const std::array<std::size_t, 2> lengths{{3000, 12000}};
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        trial("ordinary/HostRecorded bars=" + std::to_string(lengths[i]), [&, i] {
            ordinary[i] = ordinary_then_host_recorded(lengths[i]);
        });
        trial("selected KernelRecorded bars=" + std::to_string(lengths[i]), [&, i] {
            selected[i] = selected_kernel(lengths[i]);
        });
    }
    trial("growth relationship N/4N, fixed resident batch", [&] {
        MEM_NEED(ordinary[0] && ordinary[1] && selected[0] && selected[1]);
        // These compare measured container counts/bytes across runs; the +1
        // anchor is output, so subtract it before comparing bar-driven growth.
        MEM_CHECK(ordinary[1]->ordinary_points == 4 * ordinary[0]->ordinary_points);
        MEM_CHECK(ordinary[1]->ordinary_bytes == 4 * ordinary[0]->ordinary_bytes);
        MEM_CHECK(ordinary[1]->presentation_points - 1
                 == 4 * (ordinary[0]->presentation_points - 1));
        MEM_CHECK(ordinary[1]->presentation_bytes - point_bytes
                 == 4 * (ordinary[0]->presentation_bytes - point_bytes));
        MEM_CHECK(selected[1]->presentation_points - 1
                 == 4 * (selected[0]->presentation_points - 1));
        MEM_CHECK(selected[1]->presentation_bytes - point_bytes
                 == 4 * (selected[0]->presentation_bytes - point_bytes));
        // These resident array extents are fixed by type.
        MEM_CHECK(ordinary[0]->ordinary_slots == ordinary[1]->ordinary_slots);
        MEM_CHECK(ordinary[0]->presentation_slots == ordinary[1]->presentation_slots);
        MEM_CHECK(selected[0]->presentation_slots == selected[1]->presentation_slots);
    });
    std::printf("memory proof spill_equity_curve: %zu checks, %zu failures\n", checks, failures);
    return failures ? 1 : 0;
}
