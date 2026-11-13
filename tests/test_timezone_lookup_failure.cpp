#include <pineforge/native_calendar.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/selected_window_plan.hpp>
#include <pineforge/session_time.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>

#include "../src/timezone.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <new>
#include <string>
#include <system_error>

#if defined(__linux__)
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

int failures = 0;
const char* scenario = "controls";
constexpr const char* valid_zone = "America/New_York";
constexpr const char* unknown_zone = "America/Newyork";
constexpr std::int64_t first_bar = 1735779600000LL;  // 2025-01-02 01:00 UTC.
const pineforge::Bar bars[] = {
    {100, 102, 99, 101, 1, first_bar},
    {101, 103, 100, 102, 1, first_bar + 60000},
    {102, 104, 101, 103, 1, first_bar + 120000},
    {103, 105, 102, 104, 1, first_bar + 180000},
};

#define CHECK(expression) do {                                                \
    if (!(expression)) {                                                     \
        std::fprintf(stderr, "FAIL [%s] line %d: %s\n",                       \
                     scenario, __LINE__, #expression);                       \
        ++failures;                                                          \
    }                                                                        \
} while (0)

#if defined(__linux__)
enum class Call { None, Realpath, Open, Read };
enum class Header { Normal, WrongMagic, ShortEof, Split };

struct Lookup {
    Call call = Call::None;
    const char* name = valid_zone;
    int error = 0;
    unsigned remaining = 0;
    unsigned fired = 0;
    std::size_t realpaths = 0, opens = 0, reads = 0;
    int fd = -1;
    bool armed = false;
    bool directory = false;
    Header header = Header::Normal;
    unsigned header_reads = 0;
    char path[PATH_MAX] = {};
    char actual_path[PATH_MAX] = {};
};

thread_local Lookup lookup;

using RealpathFunction = char* (*)(const char*, char*);
using OpenFunction = int (*)(const char*, int, ...);
using ReadFunction = ssize_t (*)(int, void*, std::size_t);
using CheckedRealpathFunction = char* (*)(const char*, char*, std::size_t) noexcept;
using CheckedOpenFunction = int (*)(const char*, int);
using CheckedReadFunction = ssize_t (*)(int, void*, std::size_t, std::size_t);

RealpathFunction libc_realpath() noexcept {
    static const auto real = reinterpret_cast<RealpathFunction>(::dlsym(RTLD_NEXT, "realpath"));
    return real;
}

OpenFunction libc_open() noexcept {
    static const auto real = reinterpret_cast<OpenFunction>(::dlsym(RTLD_NEXT, "open"));
    return real;
}

ReadFunction libc_read() noexcept {
    static const auto real = reinterpret_cast<ReadFunction>(::dlsym(RTLD_NEXT, "read"));
    return real;
}

#if defined(__GLIBC__)
CheckedRealpathFunction libc_realpath_chk() noexcept {
    static const auto real =
        reinterpret_cast<CheckedRealpathFunction>(::dlsym(RTLD_NEXT, "__realpath_chk"));
    return real;
}

CheckedOpenFunction libc_open_2() noexcept {
    static const auto real =
        reinterpret_cast<CheckedOpenFunction>(::dlsym(RTLD_NEXT, "__open_2"));
    return real;
}

CheckedReadFunction libc_read_chk() noexcept {
    static const auto real =
        reinterpret_cast<CheckedReadFunction>(::dlsym(RTLD_NEXT, "__read_chk"));
    return real;
}
#endif

bool zone_path(const char* path) noexcept {
    if (lookup.actual_path[0] != '\0' && std::strcmp(path, lookup.actual_path) == 0)
        return true;
    const std::size_t length = std::strlen(path);
    const std::size_t suffix = std::strlen(lookup.name);
    return length > suffix && path[length - suffix - 1] == '/'
        && std::memcmp(path + length - suffix, lookup.name, suffix) == 0;
}

bool inject(Call call) noexcept {
    if (!lookup.armed || lookup.call != call || lookup.remaining == 0) return false;
    --lookup.remaining;
    ++lookup.fired;
    errno = lookup.error;
    return true;
}

const char* call_name(Call call) {
    switch (call) {
    case Call::Realpath: return "realpath";
    case Call::Open: return "open";
    case Call::Read: return "read";
    case Call::None: return "";
    }
    return "";
}

void prepare(Call call, int error, unsigned count = 1, const char* name = valid_zone) {
    lookup = {};
    lookup.call = call;
    lookup.name = name;
    lookup.error = error;
    lookup.remaining = count;
}

char* interposed_realpath(const char* path, char* resolved,
                         CheckedRealpathFunction checked = nullptr,
                         std::size_t resolved_size = 0) noexcept {
    const bool target = lookup.directory || zone_path(path);
    if (target) {
        ++lookup.realpaths;
        std::strncpy(lookup.path, path, sizeof lookup.path - 1);
        lookup.path[sizeof lookup.path - 1] = '\0';
        if (inject(Call::Realpath)) return nullptr;
    }
    char* actual = nullptr;
    if (checked) {
        actual = checked(path, resolved, resolved_size);
    } else if (const auto real = libc_realpath()) {
        actual = real(path, resolved);
    } else {
        errno = ENOSYS;
    }
    if (target && actual != nullptr) {
        // A database alias may resolve to a different file name; its later
        // open must remain part of this same targeted lookup.
        std::strncpy(lookup.actual_path, actual, sizeof lookup.actual_path - 1);
        lookup.actual_path[sizeof lookup.actual_path - 1] = '\0';
    }
    return actual;
}

int interposed_open(const char* path, int flags, bool has_mode, mode_t mode,
                    CheckedOpenFunction checked = nullptr) {
    const bool target = zone_path(path);
    if (target) {
        ++lookup.opens;
        if (inject(Call::Open)) return -1;
    }
    int fd = -1;
    if (checked) {
        fd = checked(path, flags);
    } else if (const auto real = libc_open()) {
        fd = has_mode ? real(path, flags, mode) : real(path, flags);
    } else {
        errno = ENOSYS;
    }
    if (target) lookup.fd = fd;
    return fd;
}

ssize_t interposed_read(int fd, void* buffer, std::size_t count,
                       CheckedReadFunction checked = nullptr, std::size_t buffer_size = 0) {
    bool finish_split = false;
    if (fd == lookup.fd && lookup.fd >= 0) {
        ++lookup.reads;
        if (inject(Call::Read)) return -1;
        if (lookup.armed && lookup.header != Header::Normal && count != 0) {
            const unsigned part = lookup.header_reads++;
            if (lookup.header == Header::WrongMagic) {
                const std::size_t size = count < 4 ? count : 4;
                std::memcpy(buffer, "nope", size);
                return static_cast<ssize_t>(size);
            }
            if (lookup.header == Header::ShortEof) {
                if (part != 0) return 0;
                const std::size_t size = count < 2 ? count : 2;
                std::memcpy(buffer, "TZ", size);
                return static_cast<ssize_t>(size);
            }
            if (part == 0 && count > 2) count = 2;
            finish_split = part != 0;
        }
    }
    ssize_t size = -1;
    if (checked) {
        size = checked(fd, buffer, count, buffer_size);
    } else if (const auto real = libc_read()) {
        size = real(fd, buffer, count);
    } else {
        errno = ENOSYS;
    }
    if (finish_split) lookup.header = Header::Normal;
    return size;
}
#endif

void evict_accepted_names() {
    // Eight distinct accepted offsets evict a previously accepted file name
    // from the eight-slot script memo without consulting the filesystem.
    for (const char* zone : {"UTC+1", "UTC+2", "UTC+3", "UTC+4",
                             "UTC+5", "UTC+6", "UTC+7", "UTC+8"})
        (void)pineforge::tz_util::resolve_script_timezone(std::string(zone), "hour");
}

class ZonedHost : public pineforge::source::PineStrategyHost {
public:
    std::string zone = valid_zone;
    int seen = 0;

    void on_source_bar(const pineforge::Bar& bar) override {
#if defined(__linux__)
        if (seen == 0) lookup.armed = true;
#endif
        const int hour = pineforge::local_hour(bar.timestamp, zone);
        if (seen == 0) {
            const double missing = std::numeric_limits<double>::quiet_NaN();
            strategy_entry("zone", true, missing, missing, 1.0 + hour / 1000.0);
        } else if (seen == 1) {
            strategy_close_all();
        }
        ++seen;
    }
};

template <class T>
void append_bytes(std::string& bytes, const T& value) {
    bytes.append(reinterpret_cast<const char*>(&value), sizeof value);
}

struct Outcome {
    int status = 0;
    std::string code, text, trades;
    int trade_count = 0;
    bool escaped = false, report = false;
};

Outcome run(const char* zone = valid_zone) {
    ZonedHost host;
    host.zone = zone;
    host.set_chart_timezone("UTC");
    Outcome result;
    try {
        host.run(bars, 4);
    } catch (...) {
        result.escaped = true;
    }
#if defined(__linux__)
    lookup.armed = false;
#endif
    auto& engine = static_cast<pineforge::BacktestEngine&>(host);
    const auto strategy = static_cast<pf_strategy_t>(&engine);
    result.status = strategy_last_run_status(strategy);
    const char* code = strategy_get_last_error_code(strategy);
    const char* text = strategy_get_last_error(strategy);
    result.code = code ? code : "";
    result.text = text ? text : "";
    // The harness publishes no report when any of these reads signals failure.
    if (result.escaped || result.status == 1 || !result.code.empty() || !result.text.empty())
        return result;
    pineforge::ReportC report{};
    host.fill_report(&report);
    result.report = true;
    result.trade_count = report.trades_len;
    append_bytes(result.trades, report.trades_len);
    for (int i = 0; i < report.trades_len; ++i) {
        const auto& trade = report.trades[i];
        // The complete closed-trade list, field by field without struct padding.
        append_bytes(result.trades, trade.entry_time);
        append_bytes(result.trades, trade.exit_time);
        append_bytes(result.trades, trade.entry_price);
        append_bytes(result.trades, trade.exit_price);
        append_bytes(result.trades, trade.pnl);
        append_bytes(result.trades, trade.pnl_pct);
        append_bytes(result.trades, trade.is_long);
        append_bytes(result.trades, trade.max_runup);
        append_bytes(result.trades, trade.max_drawdown);
        append_bytes(result.trades, trade.qty);
        append_bytes(result.trades, trade.commission);
        append_bytes(result.trades, trade.entry_bar_index);
        append_bytes(result.trades, trade.exit_bar_index);
        append_bytes(result.trades, trade.open_at_end);
    }
    pineforge::BacktestEngine::free_report(&report);
    return result;
}

void failure_is(const Outcome& result, const char* code, const char* call = "") {
    CHECK(!result.escaped && result.status == 1);
    CHECK(result.code == code && !result.text.empty());
    CHECK(result.text.find(call) != std::string::npos);
    CHECK(!result.report && result.trade_count == 0 && result.trades.empty());
}

void same_trades(const Outcome& result, const Outcome& clean) {
    CHECK(!result.escaped && result.status == 0 && result.report);
    CHECK(result.code.empty() && result.text.empty());
    CHECK(result.trade_count == clean.trade_count && result.trades == clean.trades);
}

std::string invalid_name_text() {
    return "Error on bar 0: Incorrect `timezone` value for the hour() function: "
        "\"America/Newyork\". The value must be a string in either UTC/GMT notation "
        "(e.g. \"UTC-5\", \"GMT+0530\") or an IANA time zone database name "
        "(e.g. \"America/New_York\").";
}

#if defined(__linux__)
void errno_rows(const Outcome& clean) {
    const struct {
        int error;
        const char* code;
    } rows[] = {
        // This table specifies the lookup contract. The first five errors mean
        // the name cannot denote a zone file on this host. ENOMEM is the memory
        // failure contract; all other lookup errors describe the host, including
        // EINTR after three attempts, rather than an incorrect timezone name.
        {ENOENT, "pine_timezone_error"},          // No file by this name.
        {ENOTDIR, "pine_timezone_error"},         // A path component is not a directory.
        {ELOOP, "pine_timezone_error"},           // The name's symlinks cannot resolve.
        {ENAMETOOLONG, "pine_timezone_error"},    // The name exceeds this host's path limits.
        {EISDIR, "pine_timezone_error"},          // A directory cannot be a zone file.
        {ENOMEM, "out_of_memory"},               // Memory exhaustion has its own code.
        {EMFILE, "engine_unclassified_error"},   // Process descriptor limit.
        {ENFILE, "engine_unclassified_error"},   // System descriptor limit.
        {EIO, "engine_unclassified_error"},      // Filesystem input/output failure.
        {EACCES, "engine_unclassified_error"},   // Host permissions could hide a valid file.
        {EINVAL, "engine_unclassified_error"},   // Operation failed, not evidence of absence.
        {ENOSYS, "engine_unclassified_error"},   // Host cannot perform the operation.
        {EINTR, "engine_unclassified_error"},    // Interrupted on all three attempts.
    };
    for (const Call call : {Call::Realpath, Call::Open, Call::Read}) {
        scenario = call_name(call);
        for (const auto& row : rows) {
            const unsigned count = row.error == EINTR ? 3 : 1;
            evict_accepted_names();
            prepare(call, row.error, count);
            const Outcome failed = run();
            CHECK(lookup.fired == count);
            const bool system_error = std::strcmp(row.code, "engine_unclassified_error") == 0;
            failure_is(failed, row.code, system_error ? call_name(call) : "");

            // Observe the script memo directly: no calendar read can disguise
            // an erroneously accepted entry. One new open proves this failed
            // name was not stored; there is deliberately no eviction here.
            prepare(Call::None, 0);
            (void)pineforge::tz_util::resolve_script_timezone(std::string(valid_zone), "hour");
            CHECK(lookup.opens == 1);
            same_trades(run(), clean);

            // The exception also preserves errno and its category before the
            // run boundary turns it into the public failure record.
            evict_accepted_names();
            prepare(call, row.error, count);
            lookup.armed = true;
            bool accepted = false, threw = false;
            try {
                (void)pineforge::tz_util::resolve_script_timezone(std::string(valid_zone), "hour");
                accepted = true;
            } catch (const std::bad_alloc& error) {
                threw = true;
                CHECK(row.error == ENOMEM);
                CHECK(dynamic_cast<const pineforge::RunFailureInfo*>(&error) != nullptr);
                CHECK(pineforge::classify_run_failure(error).code
                      == pineforge::RunFailureCode::out_of_memory);
            } catch (const std::system_error& error) {
                threw = true;
                CHECK(system_error);
                CHECK(dynamic_cast<const pineforge::RunFailureInfo*>(&error) != nullptr);
                CHECK(error.code() == std::error_code(row.error, std::generic_category()));
                CHECK(pineforge::classify_run_failure(error).code
                      == pineforge::RunFailureCode::engine_unclassified_error);
                CHECK(std::strstr(error.what(), call_name(call)) != nullptr);
            } catch (const std::exception& error) {
                threw = true;
                CHECK(std::strcmp(row.code, "pine_timezone_error") == 0);
                CHECK(pineforge::classify_run_failure(error).code
                      == pineforge::RunFailureCode::pine_timezone_error);
            }
            lookup.armed = false;
            CHECK(lookup.fired == count && !accepted && threw);
        }

        // A single interrupted call is retried and does not change the run.
        evict_accepted_names();
        prepare(call, EINTR);
        same_trades(run(), clean);
        CHECK(lookup.fired == 1);
    }
}

void posix_lookup_failure_row(const Outcome& clean) {
    scenario = "POSIX lookup fallback";
    evict_accepted_names();
    prepare(Call::Open, EMFILE, 1, "EST5EDT");
    const Outcome result = run("EST5EDT");
    CHECK(lookup.fired == 1);
    // These January bars use standard time: the engine's closed-trade output
    // for the POSIX eastern definition must equal the clean New York output.
    same_trades(result, clean);
}

void header_rows(const Outcome& clean) {
    scenario = "file header";
    for (const Header header : {Header::WrongMagic, Header::ShortEof, Header::Split}) {
        evict_accepted_names();
        prepare(Call::None, 0);
        lookup.header = header;
        const Outcome result = run();
        CHECK(lookup.header_reads > 0);
        if (header == Header::Split) {
            CHECK(lookup.header_reads == 2);
            same_trades(result, clean);
            prepare(Call::None, 0);
            lookup.header = Header::Split;
            lookup.armed = true;
            CHECK(pineforge::native_calendar::timezone_accepted(valid_zone));
            lookup.armed = false;
            CHECK(lookup.header_reads == 2);
        } else {
            failure_is(result, "pine_timezone_error");
        }
    }
}

void directory_error_rows() {
    scenario = "database directory resolution";
    for (const int error_number : {ENOMEM, EIO, EACCES, ENOENT, ENOTDIR, ELOOP}) {
        evict_accepted_names();
        prepare(Call::Realpath, error_number);
        lookup.directory = true;
        lookup.armed = true;
        bool threw = false;
        try {
            (void)pineforge::tz_util::resolve_script_timezone(std::string(valid_zone), "hour");
        } catch (const std::bad_alloc& error) {
            threw = true;
            CHECK(error_number == ENOMEM);
            CHECK(pineforge::classify_run_failure(error).code
                  == pineforge::RunFailureCode::out_of_memory);
        } catch (const std::system_error& error) {
            threw = true;
            CHECK(error_number != ENOMEM);
            CHECK(error.code() == std::error_code(error_number, std::generic_category()));
            CHECK(pineforge::classify_run_failure(error).code
                  == pineforge::RunFailureCode::engine_unclassified_error);
            CHECK(std::strstr(error.what(), "timezone database directory could not be resolved")
                  != nullptr);
            CHECK(std::strstr(error.what(), lookup.path) != nullptr);
            CHECK(std::strstr(error.what(), "realpath") != nullptr);
        } catch (const std::exception&) {
            threw = true;
            CHECK(false && "unexpected timezone database exception type");
        }
        lookup.armed = false;
        CHECK(lookup.fired == 1 && threw);
    }
}

void selected_admission_rows() {
    scenario = "selected admission";
    pineforge::SelectedPlanRequest request{};
    request.start_ms = request.fed_start_ms = first_bar;
    request.end_ms = first_bar + 240000;
    request.input_tf = request.script_tf = "1";
    request.chart_timezone = request.engine_timezone = valid_zone;
    request.session = "24x7";
    // The installed native predicate remains observational: lookup failure
    // still refuses selected admission as an unsupported engine timezone.
    for (const Call call : {Call::Realpath, Call::Open, Call::Read}) {
        for (const int error_number : {ENOMEM, EIO}) {
            prepare(call, error_number);
            lookup.armed = true;
            const auto refused = pineforge::plan_selected_primary(bars, 4, request);
            lookup.armed = false;
            CHECK(lookup.fired == 1);
            CHECK(refused.status == pineforge::SelectedPlanStatus::CalendarUnsupported);
            CHECK(refused.option == "engine_timezone");
        }
    }
    prepare(Call::None, 0);
    CHECK(pineforge::plan_selected_primary(bars, 4, request).status
          == pineforge::SelectedPlanStatus::Ok);
    request.chart_timezone = request.engine_timezone = unknown_zone;
    const auto refused = pineforge::plan_selected_primary(bars, 4, request);
    CHECK(refused.status == pineforge::SelectedPlanStatus::CalendarUnsupported);
    CHECK(refused.option == "engine_timezone");
}
#endif

#if defined(__GLIBC__)
void missing_directory_rows(const Outcome& clean) {
    scenario = "non-directory database";
    // Japan is a bare, non-POSIX alias already required by test_native_calendar.
    // Ubuntu and the hosted CI images install tzdata-legacy for that alias.
    CHECK(pineforge::native_calendar::timezone_accepted("Japan"));
    const char* previous = std::getenv("TZDIR");
    const bool was_set = previous != nullptr;
    const std::string saved = previous ? previous : "";
    // /dev/null is a file on these hosts, never a timezone database directory.
    CHECK(::setenv("TZDIR", "/dev/null", 1) == 0);
    evict_accepted_names();
#if defined(__linux__)
    prepare(Call::None, 0);
#endif
    const Outcome result = run();
    failure_is(result, "engine_unclassified_error", "timezone database directory could not be resolved");
    CHECK(result.text.find("/dev/null") != std::string::npos);
    const Outcome offset = run("UTC-5");
    same_trades(offset, clean);  // New York is also UTC-5 on these January bars.
    // The POSIX eastern definition also agrees with New York on these January
    // bars; compare the engine's complete closed-trade output to prove it.
    same_trades(run("EST5EDT"), clean);
    const Outcome bare_file = run("Japan");
    failure_is(bare_file, "engine_unclassified_error",
               "timezone database directory could not be resolved");
    CHECK(bare_file.text.find("/dev/null") != std::string::npos);
    for (const char* zone : {"UTC", "GMT", "Etc/UTC", "Etc/GMT", "UTC-5", "GMT+0530"})
        CHECK(pineforge::native_calendar::timezone_accepted(zone));
    const int restored = was_set ? ::setenv("TZDIR", saved.c_str(), 1) : ::unsetenv("TZDIR");
    CHECK(restored == 0);
    // Do not evict here: recovery must validate the name that failed while the
    // directory was unusable, rather than finding an accepted memo entry.
#if defined(__linux__)
    prepare(Call::None, 0);
#endif
    (void)pineforge::tz_util::resolve_script_timezone(std::string(valid_zone), "hour");
#if defined(__linux__)
    CHECK(lookup.opens == 1);
#endif
    same_trades(run(), clean);
}
#else
// Non-glibc libcs do not promise that TZDIR selects their timezone database.
// The non-directory TZDIR row is compiled out on those platforms.
#endif

}  // namespace

#if defined(__linux__)
// Optimized glibc headers can route realpath with a known buffer size through
// __realpath_chk, read through __read_chk, and open with nonconstant flags
// through __open_2. Both forms use the same counting, injection and forwarding
// body. Separate C++ names for the plain exports avoid redefining the headers'
// inline wrappers; their ELF symbols remain realpath, open and read.
extern "C" char* realpath_interposer(const char*, char*) noexcept __asm__("realpath");
extern "C" int open_interposer(const char*, int, ...) __asm__("open");
extern "C" ssize_t read_interposer(int, void*, std::size_t) __asm__("read");

extern "C" char* realpath_interposer(const char* path, char* resolved) noexcept {
    return interposed_realpath(path, resolved);
}

extern "C" int open_interposer(const char* path, int flags, ...) {
    mode_t mode = 0;
    bool has_mode = (flags & O_CREAT) != 0;
#if defined(O_TMPFILE)
    has_mode = has_mode || (flags & O_TMPFILE) == O_TMPFILE;
#endif
    if (has_mode) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }
    return interposed_open(path, flags, has_mode, mode);
}

extern "C" ssize_t read_interposer(int fd, void* buffer, std::size_t count) {
    return interposed_read(fd, buffer, count);
}

#if defined(__GLIBC__)
extern "C" char* __realpath_chk(const char* __restrict path, char* __restrict resolved,
                              std::size_t resolved_size) noexcept {
    return interposed_realpath(path, resolved, libc_realpath_chk(), resolved_size);
}

extern "C" ssize_t __read_chk(int fd, void* buffer, std::size_t count, std::size_t buffer_size) {
    return interposed_read(fd, buffer, count, libc_read_chk(), buffer_size);
}

extern "C" int __open_2(const char* path, int flags) {
    return interposed_open(path, flags, false, 0, libc_open_2());
}
#endif
#else
// The syscall injection rows require Linux ELF symbol interposition and
// RTLD_NEXT, matching the existing timezone setenv-interposition test.
// Other platforms compile the clean-run and invalid-name controls below.
#endif

int main() {
#if defined(__linux__)
    // Resolve the forwarding functions before any injection is armed.
    CHECK(libc_realpath() != nullptr && libc_open() != nullptr && libc_read() != nullptr);
#if defined(__GLIBC__)
    (void)libc_realpath_chk();
    (void)libc_open_2();
    (void)libc_read_chk();
#endif
#endif
    evict_accepted_names();
    const Outcome clean = run();
    CHECK(clean.report && clean.status == 0 && clean.code.empty() && clean.text.empty());
    CHECK(clean.trade_count == 1 && !clean.trades.empty());
    const Outcome invalid = run(unknown_zone);
    failure_is(invalid, "pine_timezone_error");
    CHECK(invalid.text == invalid_name_text());
    same_trades(run(), clean);
#if defined(__linux__)
    errno_rows(clean);
    posix_lookup_failure_row(clean);
    header_rows(clean);
    directory_error_rows();
    selected_admission_rows();
#endif
#if defined(__GLIBC__)
    missing_directory_rows(clean);
#endif
    return failures == 0 ? 0 : 1;
}
