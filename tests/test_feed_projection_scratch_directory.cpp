#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <pineforge/detail/feed_projection.hpp>
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/pineforge.h>
#include <pineforge/report_sink.h>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/timeframe.hpp>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <ctime>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace feed_projection_scratch_directory_test {
using namespace pineforge;
using TapeRow = source::RecordedSeriesRow;
using Tape = detail::FeedProjection<TapeRow>;
using Calendar = detail::FeedProjection<std::int64_t>;

constexpr std::int64_t first_stamp = 1704067200000LL;
constexpr std::int64_t day_ms = 24 * 60 * 60 * 1000LL;
int failures = 0;

#define CHECK(condition) do { if (!(condition)) { \
    std::printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; \
} } while (false)

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Directory {
    std::string path;

    explicit Directory(const char* prefix) {
        std::string pattern = std::string("/var/tmp/") + prefix + "-XXXXXX";
        std::vector<char> bytes(pattern.begin(), pattern.end());
        bytes.push_back('\0');
        require(::mkdtemp(bytes.data()) != nullptr, "cannot create projection directory");
        path = bytes.data();
    }
    ~Directory() { ::rmdir(path.c_str()); }
    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;
};

struct OwnedFile {
    std::string path;
    ~OwnedFile() {
        if (!path.empty()) ::unlink(path.c_str());
    }
};

OwnedFile make_report_path(const Directory& directory) {
    std::string pattern = directory.path + "/report-XXXXXX";
    std::vector<char> bytes(pattern.begin(), pattern.end());
    bytes.push_back('\0');
    const int fd = ::mkstemp(bytes.data());
    require(fd >= 0, "cannot create report path");
    ::close(fd);
    return {bytes.data()};
}

class Environment final {
public:
    Environment(const char* name, const std::string& value) : name_(name) {
        if (const char* previous = std::getenv(name_)) {
            had_previous_ = true;
            previous_ = previous;
        }
        require(::setenv(name_, value.c_str(), 1) == 0, "cannot set scratch environment");
    }
    ~Environment() {
        if (had_previous_) ::setenv(name_, previous_.c_str(), 1);
        else ::unsetenv(name_);
    }
    Environment(const Environment&) = delete;
    Environment& operator=(const Environment&) = delete;
private:
    const char* name_;
    bool had_previous_ = false;
    std::string previous_;
};

class JailedArena final {
public:
    explicit JailedArena(std::uint64_t bytes)
        : fd_environment_("PF_SCRATCH_ARENA_FD", "5"),
          bytes_environment_("PF_SCRATCH_ARENA_BYTES", std::to_string(bytes)),
          profile_environment_("PF_SCRATCH_ARENA_JAILED", "1") {
        require(detail::ScratchArena::attached() == nullptr,
                "projection arena fixture starts detached");
        saved_fd_ = ::fcntl(detail::ScratchArena::kInheritedFd, F_DUPFD_CLOEXEC, 16);
        require(saved_fd_ >= 0 || errno == EBADF,
                "cannot preserve the inherited projection arena slot");
        arena_fd_ = ::memfd_create("pineforge-feed-arena", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        require(arena_fd_ >= 0, "cannot create the jailed projection arena");
        require(::ftruncate(arena_fd_, static_cast<off_t>(bytes)) == 0,
                "cannot size the jailed projection arena");
        require(::fcntl(arena_fd_, F_ADD_SEALS,
                        F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0,
                "cannot seal the jailed projection arena");
        require(::dup2(arena_fd_, detail::ScratchArena::kInheritedFd)
                    == detail::ScratchArena::kInheritedFd,
                "cannot install the inherited projection arena");
    }
    ~JailedArena() {
        if (detail::ScratchArena::attached()) detail::ScratchArena::detach();
        ::close(detail::ScratchArena::kInheritedFd);
        if (saved_fd_ >= 0) {
            ::dup2(saved_fd_, detail::ScratchArena::kInheritedFd);
            ::close(saved_fd_);
        }
        if (arena_fd_ >= 0 && arena_fd_ != detail::ScratchArena::kInheritedFd)
            ::close(arena_fd_);
    }
    JailedArena(const JailedArena&) = delete;
    JailedArena& operator=(const JailedArena&) = delete;

private:
    Environment fd_environment_;
    Environment bytes_environment_;
    Environment profile_environment_;
    int saved_fd_ = -1;
    int arena_fd_ = -1;
};

class CreationWatch final {
public:
    CreationWatch(const std::string& configured, const std::string& default_directory)
        : fd_(::inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) {
        require(fd_ >= 0, "cannot create projection directory watch");
        configured_watch_ = ::inotify_add_watch(fd_, configured.c_str(), IN_CREATE);
        default_watch_ = ::inotify_add_watch(fd_, default_directory.c_str(), IN_CREATE);
        require(configured_watch_ >= 0 && default_watch_ >= 0,
                "cannot watch projection directories");
    }
    ~CreationWatch() {
        if (configured_watch_ >= 0) ::inotify_rm_watch(fd_, configured_watch_);
        if (default_watch_ >= 0) ::inotify_rm_watch(fd_, default_watch_);
        if (fd_ >= 0) ::close(fd_);
    }
    CreationWatch(const CreationWatch&) = delete;
    CreationWatch& operator=(const CreationWatch&) = delete;

    void collect() {
        std::array<char, 16384> bytes{};
        for (;;) {
            const ssize_t count = ::read(fd_, bytes.data(), bytes.size());
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && errno == EAGAIN) return;
            require(count > 0, "cannot read projection directory watch");
            std::size_t offset = 0;
            while (offset < static_cast<std::size_t>(count)) {
                require(static_cast<std::size_t>(count) - offset >= sizeof(inotify_event),
                        "short projection directory event");
                const auto* event = reinterpret_cast<const inotify_event*>(
                    bytes.data() + offset);
                require(event->len <= static_cast<std::size_t>(count) - offset
                            - sizeof(inotify_event),
                        "invalid projection directory event");
                if ((event->mask & IN_CREATE) && event->len != 0) {
                    const std::string name(event->name);
                    if (name.rfind("pineforge-feed-", 0) == 0) {
                        if (event->wd == configured_watch_) ++configured_feed_creations_;
                        if (event->wd == default_watch_) ++default_feed_creations_;
                    }
                }
                offset += sizeof(inotify_event) + event->len;
            }
        }
    }
    std::size_t configured_feed_creations() const noexcept {
        return configured_feed_creations_;
    }
    std::size_t default_feed_creations() const noexcept {
        return default_feed_creations_;
    }
private:
    int fd_ = -1;
    int configured_watch_ = -1;
    int default_watch_ = -1;
    std::size_t configured_feed_creations_ = 0;
    std::size_t default_feed_creations_ = 0;
};

double tape_value(std::size_t index) {
    if (index == 0) return -0.0;
    return static_cast<double>((index * 37) % 257 - 128) * 0.125;
}

bool same_bits(double left, double right) {
    return std::memcmp(&left, &right, sizeof(left)) == 0;
}

Bar bar_at(std::int64_t timestamp, std::size_t index) {
    const double price = 100.0 + static_cast<double>(index) * 0.25;
    return {price, price + 1.0, price - 1.0, price + 0.5, 1.0, timestamp};
}

struct Input {
    std::vector<std::int64_t> stamps;
    std::vector<Bar> daily;
    std::vector<std::int64_t> tape_opens;
    std::vector<double> tape_values;
};

struct ProjectionDemand {
    std::size_t calendar_rows;
    std::size_t tape_rows;
    std::uint64_t calendar_column_bytes;
    std::uint64_t tape_bytes;
    std::uint64_t arena_bytes;
};

ProjectionDemand projection_demand() {
    const auto calendar_rows = Calendar::rows_per_page() + 1;
    const auto tape_rows = Tape::rows_per_page() + 1;
    const auto calendar_column_bytes =
        static_cast<std::uint64_t>(calendar_rows) * sizeof(std::int64_t);
    const auto tape_bytes =
        static_cast<std::uint64_t>(tape_rows) * sizeof(TapeRow);
    const auto chunk = detail::ScratchArena::kChunkBytes;
    const auto chunks_for = [chunk](std::uint64_t bytes) {
        return bytes / chunk + (bytes % chunk != 0 ? 1 : 0);
    };
    // Each of the three calendar columns owns a separate projection region.
    const auto chunks = 3 * chunks_for(calendar_column_bytes) + chunks_for(tape_bytes);
    return {calendar_rows, tape_rows, calendar_column_bytes, tape_bytes, chunks * chunk};
}

Input make_input(std::size_t calendar_rows, std::size_t tape_rows) {
    Input input;
    input.stamps.reserve(calendar_rows);
    input.daily.reserve(calendar_rows);
    for (std::size_t i = 0; i < calendar_rows; ++i) {
        const auto timestamp = first_stamp + static_cast<std::int64_t>(i) * day_ms;
        input.stamps.push_back(timestamp);
        input.daily.push_back(bar_at(timestamp, i));
    }
    input.tape_opens.reserve(tape_rows);
    input.tape_values.reserve(tape_rows);
    for (std::size_t i = 0; i < tape_rows; ++i) {
        input.tape_opens.push_back(first_stamp + static_cast<std::int64_t>(i) * day_ms);
        input.tape_values.push_back(tape_value(i));
    }
    return input;
}

class ProjectionHost final : public source::PineStrategyHost {
public:
    ProjectionHost() {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_type("crypto");
        set_syminfo_string("tickerid", "TEST:BASE");
        set_syminfo_mintick(0.01);
        source::PineStrategyConfig config;
        config.initial_capital = 100000.0;
        configure_pine_strategy(config);
    }

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        register_security_eval(0, "D", "360", false, false);
    }
    void evaluate_security(int, const Bar&, bool) override {}
    void clear_security(int) override {}
    void on_source_bar(const Bar&) override {}

    const NativeDayPartition& calendar() const noexcept { return chart_day_partition_; }
    double tape_at(std::int64_t timestamp) {
        current_bar_.timestamp = timestamp;
        return recorded_series_value("tape");
    }
    pf_strategy_t handle() noexcept {
        return static_cast<BacktestEngine*>(this);
    }
};

void check_tape(const ProjectionHost& host, const Input& input) {
    auto& readable = const_cast<ProjectionHost&>(host);
    for (std::size_t i = 0; i < input.tape_opens.size(); ++i)
        CHECK(same_bits(readable.tape_at(input.tape_opens[i]), input.tape_values[i]));
}

void check_calendar(const NativeDayPartition& partition, const Input& input) {
    CHECK(partition.stamps == input.stamps);
    CHECK(partition.trade_day.size() == input.stamps.size());
    CHECK(partition.week_open.size() == input.stamps.size());
    CHECK(partition.month_open.size() == input.stamps.size());
    for (std::size_t i = 0; i < input.stamps.size(); ++i) {
        const auto day = input.stamps[i] / kMsPerDay;
        const auto week = input.stamps[i - i % 7];
        const time_t seconds = static_cast<time_t>(input.stamps[i] / 1000);
        struct tm date {};
        require(::gmtime_r(&seconds, &date) != nullptr, "cannot derive calendar row");
        const auto month = input.stamps[i]
            - static_cast<std::int64_t>(date.tm_mday - 1) * kMsPerDay;
        CHECK(partition.trade_day[i] == day);
        CHECK(partition.week_open[i] == week);
        CHECK(partition.month_open[i] == month);
    }
}

void configure_projection_sink(ProjectionHost& host, const Directory& configured,
                               const OwnedFile& report) {
    const pf_report_sink_v1 descriptor{
        sizeof(descriptor), 1, report.path.c_str(), "feed-projection-directory",
        configured.path.c_str()};
    require(host.set_report_sink_v1(&descriptor) == 0,
            "projection sink configuration failed");
}

void configured_directory_case() {
    Directory configured("pineforge-feed-configured");
    Directory default_directory("pineforge-feed-default");
    Directory report_directory("pineforge-feed-report");
    OwnedFile report = make_report_path(report_directory);
    Environment temporary("TMPDIR", default_directory.path);
    CreationWatch watch(configured.path, default_directory.path);

    const auto calendar_rows = Calendar::rows_per_page() + 1;
    const auto tape_rows = Tape::rows_per_page() + 1;
    const auto input = make_input(calendar_rows, tape_rows);
    ProjectionHost host;
    const pf_report_sink_v1 descriptor{
        sizeof(descriptor), 1, report.path.c_str(), "feed-projection-directory",
        configured.path.c_str()};
    CHECK(host.set_report_sink_v1(&descriptor) == 0);
    CHECK(host.set_native_security_feed("D", input.daily.data(),
                                        static_cast<int>(input.daily.size())));
    CHECK(host.set_recorded_series("tape", input.tape_opens.data(), input.tape_values.data(),
                                   static_cast<int>(input.tape_opens.size())));
    host.run(input.daily.data(), static_cast<int>(input.daily.size()), "360", "360",
             false, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    watch.collect();

    // One raw recorded-series builder plus the three calendar builders must
    // each spill: the row counts above are one past their derived page sizes.
    CHECK(watch.configured_feed_creations() == 4);
    CHECK(watch.default_feed_creations() == 0);
    check_tape(host, input);
    check_calendar(host.calendar(), input);
}

void configured_spill_failure_case() {
    Directory default_directory("pineforge-feed-default-failure");
    Environment temporary("TMPDIR", default_directory.path);
    char unavailable[] = "/tmp/pineforge-feed-reader-XXXXXX";
    require(::mkdtemp(unavailable) != nullptr, "cannot create unavailable directory");
    require(::rmdir(unavailable) == 0, "cannot remove unavailable directory");
    CreationWatch watch(default_directory.path, default_directory.path);

    detail::FeedProjectionBuilder<std::int64_t> builder(unavailable, true);
    for (std::size_t i = 0; i < Calendar::rows_per_page(); ++i)
        builder.append(static_cast<std::int64_t>(i));
    bool typed_failure = false;
    try {
        builder.append(static_cast<std::int64_t>(Calendar::rows_per_page()));
    } catch (const detail::AuxiliaryScratchIoError& error) {
        typed_failure = error.code().value() == ENOENT;
    }
    CHECK(typed_failure);
    watch.collect();
    CHECK(watch.default_feed_creations() == 0);
}

void jailed_projection_success_case() {
    const auto demand = projection_demand();
    std::printf("projection arena demand calendar_column_bytes=%llu tape_bytes=%llu "
                "arena_bytes=%llu\n",
                static_cast<unsigned long long>(demand.calendar_column_bytes),
                static_cast<unsigned long long>(demand.tape_bytes),
                static_cast<unsigned long long>(demand.arena_bytes));
    JailedArena arena(demand.arena_bytes);
    Directory configured("pineforge-feed-jailed-configured");
    Directory default_directory("pineforge-feed-jailed-default");
    Directory report_directory("pineforge-feed-jailed-report");
    OwnedFile report = make_report_path(report_directory);
    Environment temporary("TMPDIR", default_directory.path);
    CreationWatch watch(configured.path, default_directory.path);

    ProjectionHost host;
    require(strategy_require_report_scratch_arena_v1(host.handle()) == 0,
            "jailed projection arena admission failed");
    configure_projection_sink(host, configured, report);
    const auto input = make_input(demand.calendar_rows, demand.tape_rows);
    NativeDayPartition calendar;
    require(build_native_day_partition(
                calendar, "UTC", "24x7", input.stamps, input.daily.data(),
                static_cast<int>(input.daily.size()), configured.path),
            "jailed calendar projection build failed");
    CHECK(host.set_recorded_series("tape", input.tape_opens.data(), input.tape_values.data(),
                                   static_cast<int>(input.tape_opens.size())));
    CHECK(host.last_error().empty());
    watch.collect();
    CHECK(watch.configured_feed_creations() == 0);
    CHECK(watch.default_feed_creations() == 0);
    check_calendar(calendar, input);
    check_tape(host, input);
}

void jailed_projection_exhaustion_case() {
    const auto demand = projection_demand();
    JailedArena arena(demand.arena_bytes - 1);
    Directory configured("pineforge-feed-jailed-failure-configured");
    Directory default_directory("pineforge-feed-jailed-failure-default");
    Directory report_directory("pineforge-feed-jailed-failure-report");
    OwnedFile report = make_report_path(report_directory);
    Environment temporary("TMPDIR", default_directory.path);
    CreationWatch watch(configured.path, default_directory.path);

    ProjectionHost host;
    require(strategy_require_report_scratch_arena_v1(host.handle()) == 0,
            "jailed projection failure admission failed");
    configure_projection_sink(host, configured, report);
    const auto input = make_input(demand.calendar_rows, demand.tape_rows);
    NativeDayPartition calendar;
    require(build_native_day_partition(
                calendar, "UTC", "24x7", input.stamps, input.daily.data(),
                static_cast<int>(input.daily.size()), configured.path),
            "jailed calendar projection control failed");
    bool typed_exhaustion = false;
    try {
        (void)host.set_recorded_series("tape", input.tape_opens.data(),
                                       input.tape_values.data(),
                                       static_cast<int>(input.tape_opens.size()));
    } catch (const detail::ReportScratchIoError& error) {
        const auto failure = classify_run_failure(error);
        typed_exhaustion = error.code() == std::error_code(EFBIG, std::generic_category())
            && failure.code == RunFailureCode::report_sink_failed
            && failure.args
            && failure.args->find("\"reason\":\"arena_exhausted\"") != std::string::npos;
    }
    CHECK(typed_exhaustion);
    watch.collect();
    CHECK(watch.configured_feed_creations() == 0);
    CHECK(watch.default_feed_creations() == 0);
}

void default_small_case() {
    Directory default_directory("pineforge-feed-default-small");
    Environment temporary("TMPDIR", default_directory.path);
    CreationWatch watch(default_directory.path, default_directory.path);

    const auto input = make_input(1, 1);
    ProjectionHost host;
    CHECK(host.set_native_security_feed("D", input.daily.data(),
                                        static_cast<int>(input.daily.size())));
    CHECK(host.set_recorded_series("tape", input.tape_opens.data(), input.tape_values.data(),
                                   static_cast<int>(input.tape_opens.size())));
    host.run(input.daily.data(), static_cast<int>(input.daily.size()), "360", "360",
             false, 4, MagnifierDistribution::ENDPOINTS);
    CHECK(host.last_error().empty());
    watch.collect();

    // The recommendation keeps a one-page projection resident, so this
    // nonempty default run must not attempt a projection backing file.
    CHECK(watch.default_feed_creations() == 0);
    check_tape(host, input);
    check_calendar(host.calendar(), input);
}
} // namespace feed_projection_scratch_directory_test

int main() {
    using namespace feed_projection_scratch_directory_test;
    try {
        configured_directory_case();
        configured_spill_failure_case();
        jailed_projection_success_case();
        jailed_projection_exhaustion_case();
        default_small_case();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "projection scratch-directory setup failed: %s\n", error.what());
        return EXIT_FAILURE;
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
