// Deferred sink sync: a path sink still writes every 64 KiB window as it fills
// (pwrite of the aligned window, then ftruncate to the logical end) but makes
// the file durable once, inside write_trailer or abandon, instead of once per
// window. Linux --wrap accounting of the real writer and of a real native run;
// production code has no fault hook. Expected values come from the version-1
// wire format, the FIPS 180-2 SHA-256 vector and the 64 KiB window rule.
// Against the per-window writer, count, sync_fault, write_fault,
// truncate_fault, engine, engine_sync_fault and abandon fail a sync check (the
// fault rows after their unchanged type and text checks); bytes and memory pass
// on both writers.
#include "report_sink_failure_fixture.hpp"
#include "../src/report_sink_writer.hpp"
#include "../src/sha256.hpp"

#include <pineforge/native_host.hpp>
#include <pineforge/version.h>

#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

namespace allocation_probe {
struct alignas(std::max_align_t) Header { std::size_t size; bool counted; };
bool enabled = false;
std::size_t calls = 0, traffic = 0, retained = 0, peak = 0;
}

void* operator new(std::size_t size) {
    using namespace allocation_probe;
    auto* header = static_cast<Header*>(std::malloc(sizeof(Header) + size));
    if (!header) throw std::bad_alloc();
    header->size = size;
    header->counted = enabled;
    if (enabled) {
        ++calls;
        traffic += size;
        retained += size;
        if (retained > peak) peak = retained;
    }
    return header + 1;
}
void operator delete(void* value) noexcept {
    if (!value) return;
    auto* header = static_cast<allocation_probe::Header*>(value) - 1;
    if (header->counted) allocation_probe::retained -= header->size;
    std::free(header);
}
// The nothrow forms must come from the same allocator: a sanitizer runtime intercepts
// them, and then the matching replaced delete would read a header that was never made.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* value) noexcept { ::operator delete(value); }
void operator delete(void* value, std::size_t) noexcept { ::operator delete(value); }
void operator delete[](void* value, std::size_t) noexcept { ::operator delete(value); }

namespace {
namespace sinksync {

enum class Op : unsigned char { pwrite, ftruncate, fdatasync, fsync };
constexpr std::array<const char*, 4> kOpNames{{"pwrite", "ftruncate", "fdatasync", "fsync"}};

struct Event {
    Op op;
    long long position;  // pwrite offset, or ftruncate length
    std::size_t bytes;   // pwrite request
    long long result;
    int error;
};

// Fixed bookkeeping: every row's flush count is far below the event bound.
struct Probe {
    bool armed = false;
    dev_t device = 0;
    ino_t inode = 0;
    std::array<char, 4096> directory{};
    std::size_t directory_length = 0;
    bool capture_sync_bytes = false, snapshot_ok = false;
    bool close_after_sync = false, close_ok = false;
    std::string sync_bytes;
    std::array<Event, 1024> events{};
    std::size_t count = 0;
    bool overflow = false;
    std::array<std::size_t, 4> calls{};  // the sink file's calls, failed ones included
    std::array<std::size_t, 4> any{};    // calls on every descriptor while armed
    bool inject = false;
    Op fault = Op::fdatasync;
    std::size_t ordinal = 0;             // which call of `fault` fails, from one
    int error = 0;
};
Probe probe;

std::size_t slot(Op op) { return static_cast<std::size_t>(op); }

// Follow an installed stream inode, or the private sibling opened in the
// path row's directory. Scratch files never enter the sink's sequence.
void arm(dev_t device, ino_t inode) {
    probe = Probe{};
    probe.device = device;
    probe.inode = inode;
    probe.armed = true;
}

void inject(Op op, std::size_t ordinal, int error) {
    probe.inject = true;
    probe.fault = op;
    probe.ordinal = ordinal;
    probe.error = error;
}

// Classify one call before it runs; true when it must fail as injected.
bool enter(Op op, int fd, bool& mine) {
    mine = false;
    if (!probe.armed) return false;
    ++probe.any[slot(op)];
    struct stat info{};
    mine = ::fstat(fd, &info) == 0 && info.st_dev == probe.device && info.st_ino == probe.inode;
    if (!mine && probe.directory_length != 0 && S_ISREG(info.st_mode)) {
        char link[64];
        std::snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
        std::array<char, 4096> target{};
        const auto size = ::readlink(link, target.data(), target.size() - 1);
        if (size >= 0 && static_cast<std::size_t>(size) < target.size() - 1) {
            const auto* slash = std::strrchr(target.data(), '/');
            if (slash && std::string_view(target.data(), slash - target.data())
                    == std::string_view(probe.directory.data(), probe.directory_length)
                && std::strncmp(slash + 1, ".pf-report-sink-", 16) == 0) {
                probe.device = info.st_dev;
                probe.inode = info.st_ino;
                probe.directory_length = 0;
                mine = true;
            }
        }
    }
    if (!mine) return false;
    const auto ordinal = ++probe.calls[slot(op)];
    return probe.inject && probe.fault == op && ordinal == probe.ordinal;
}

void note(Op op, long long position, std::size_t bytes, long long result, int error) {
    if (probe.count == probe.events.size()) {
        probe.overflow = true;
        return;
    }
    probe.events[probe.count++] = Event{op, position, bytes, result, error};
}

// Inspect complete bytes at the sync boundary, before cleanup removes the
// private sibling. This is never enabled by the allocation-measurement rows.
void snapshot(int fd) {
    const int saved = errno;
    char link[64];
    std::snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    const int reader = ::open(link, O_RDONLY | O_CLOEXEC);
    probe.sync_bytes.clear();
    probe.snapshot_ok = false;
    if (reader >= 0) {
        std::array<char, 65536> buffer{};
        for (;;) {
            const auto got = ::read(reader, buffer.data(), buffer.size());
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) {
                probe.snapshot_ok = got == 0;
                break;
            }
            probe.sync_bytes.append(buffer.data(), static_cast<std::size_t>(got));
        }
        ::close(reader);
    }
    errno = saved;
}
} // namespace sinksync

// Observe creation, permission changes and every actual data write without
// allocating inside the writer's measurement interval.
namespace sinkmode {
struct Probe {
    bool armed = false, stat_failed = false;
    bool bad_creation = false, bad_write_mode = false;
    dev_t directory_device = 0, device = 0;
    ino_t directory_inode = 0, inode = 0;
    mode_t observed = 0;
    std::size_t creations = 0, observations = 0, writes = 0, chmods = 0;
};
Probe probe;

bool observe(int fd) {
    if (!probe.armed || probe.creations == 0) return false;
    const int saved = errno;
    struct stat info{};
    const bool ok = ::fstat(fd, &info) == 0;
    if (!ok) probe.stat_failed = true;
    const bool mine = ok && info.st_dev == probe.device && info.st_ino == probe.inode;
    if (mine) {
        probe.observed |= info.st_mode & 0777;
        ++probe.observations;
    }
    errno = saved;
    return mine;
}

void opened(int directory, int fd, int flags, mode_t requested) {
    if (!probe.armed || fd < 0) return;
    const int saved = errno;
    struct stat parent{}, file{};
    if (::fstat(directory, &parent) != 0 || ::fstat(fd, &file) != 0) {
        probe.stat_failed = true;
    } else if (parent.st_dev == probe.directory_device
               && parent.st_ino == probe.directory_inode && S_ISREG(file.st_mode)) {
        probe.device = file.st_dev;
        probe.inode = file.st_ino;
        probe.bad_creation = requested != 0600 || (flags & O_EXCL) == 0;
        ++probe.creations;
        observe(fd); // Before control returns to the writer's fchmod.
    }
    errno = saved;
}

struct Watch {
    explicit Watch(const std::string& directory) {
        struct stat info{};
        cr150_w1::require(::stat(directory.c_str(), &info) == 0,
                         "stat the permission fixture directory");
        probe = {};
        probe.directory_device = info.st_dev;
        probe.directory_inode = info.st_ino;
        probe.armed = true;
    }
    ~Watch() { probe.armed = false; }
};
} // namespace sinkmode
} // namespace

extern "C" int __real_openat(int, const char*, int, ...);
extern "C" int __real_fchmod(int, mode_t);
extern "C" ssize_t __real_pwrite(int, const void*, std::size_t, off_t);
extern "C" int __real_ftruncate(int, off_t);
extern "C" int __real_fdatasync(int);
extern "C" int __real_fsync(int);

extern "C" int __wrap_openat(int directory, const char* path, int flags, ...) {
    mode_t mode = 0;
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
    }
    const int result = __real_openat(directory, path, flags, mode);
    const int saved = errno;
    if (flags & O_CREAT) sinkmode::opened(directory, result, flags, mode);
    errno = saved;
    return result;
}
extern "C" int __wrap_fchmod(int fd, mode_t mode) {
    if (sinkmode::observe(fd)) ++sinkmode::probe.chmods;
    const int result = __real_fchmod(fd, mode);
    const int saved = errno;
    sinkmode::observe(fd);
    errno = saved;
    return result;
}
extern "C" ssize_t __wrap_pwrite(int fd, const void* data, std::size_t bytes, off_t offset) {
    using namespace sinksync;
    if (sinkmode::observe(fd)) {
        ++sinkmode::probe.writes;
        struct stat info{};
        if (::fstat(fd, &info) != 0 || (info.st_mode & 0777) != 0600)
            sinkmode::probe.bad_write_mode = true;
    }
    bool mine = false;
    if (enter(Op::pwrite, fd, mine)) {
        note(Op::pwrite, offset, bytes, -1, probe.error);
        errno = probe.error;
        return -1;
    }
    const auto result = __real_pwrite(fd, data, bytes, offset);
    const int error = result < 0 ? errno : 0;
    if (mine) note(Op::pwrite, offset, bytes, result, error);
    if (result < 0) errno = error;
    return result;
}
extern "C" int __wrap_ftruncate(int fd, off_t length) {
    using namespace sinksync;
    bool mine = false;
    if (enter(Op::ftruncate, fd, mine)) {
        note(Op::ftruncate, length, 0, -1, probe.error);
        errno = probe.error;
        return -1;
    }
    const int result = __real_ftruncate(fd, length);
    const int error = result != 0 ? errno : 0;
    if (mine) note(Op::ftruncate, length, 0, result, error);
    if (result != 0) errno = error;
    return result;
}
extern "C" int __wrap_fdatasync(int fd) {
    using namespace sinksync;
    bool mine = false;
    const bool injected = enter(Op::fdatasync, fd, mine);
    if (mine && probe.capture_sync_bytes) snapshot(fd);
    if (injected) {
        note(Op::fdatasync, 0, 0, -1, probe.error);
        errno = probe.error;
        return -1;
    }
    const int result = __real_fdatasync(fd);
    const int error = result != 0 ? errno : 0;
    if (mine) note(Op::fdatasync, 0, 0, result, error);
    // Force a real EBADF at the checked close after a real successful sync.
    if (mine && result == 0 && probe.close_after_sync) probe.close_ok = ::close(fd) == 0;
    if (result != 0) errno = error;
    return result;
}
extern "C" int __wrap_fsync(int fd) {
    using namespace sinksync;
    bool mine = false;
    if (enter(Op::fsync, fd, mine)) {
        note(Op::fsync, 0, 0, -1, probe.error);
        errno = probe.error;
        return -1;
    }
    const int result = __real_fsync(fd);
    const int error = result != 0 ? errno : 0;
    if (mine) note(Op::fsync, 0, 0, result, error);
    if (result != 0) errno = error;
    return result;
}

namespace {
using namespace cr150_w1;
using sinksync::Op;
using sinksync::probe;
using sinksync::slot;
namespace rs = pineforge::report_sink;

constexpr std::size_t kWindow = 64 * 1024;
constexpr std::size_t kAlign = 4096;
constexpr std::size_t kRows = 9000;  // E rows: more than four windows
constexpr int kEngineBars = 16384;   // about 31 bytes of E per bar: seven windows
constexpr char kRunId[] = "deferred-sink-sync";
constexpr std::string_view kTrailerStart = "{\"trailer\":\"pf-report-sink\"";
const rs::RecordSet kRecords{false, false, false, 0, 0};

// ---- The known answer: the version-1 wire format, built independently ----

std::string header_line() {
    const std::string engine = PINEFORGE_VERSION_STRING;
    for (const char c : engine) {
        const auto byte = static_cast<unsigned char>(c);
        require(c != '"' && c != '\\' && byte >= 0x20 && byte < 0x80,
                "the known answer assumes an engine version without JSON escapes");
    }
    return std::string("{\"schema\":\"pf-report-sink\",\"version\":1,\"run_id\":\"") + kRunId
        + "\",\"engine\":\"" + engine + "\",\"records\":["
        "{\"type\":\"E\",\"fields\":[\"time_ms\",\"equity\",\"open_profit\"],"
        "\"types\":[\"i64\",\"f64?\",\"f64?\"],"
        "\"units\":[\"unix_ms\",\"account_ccy\",\"account_ccy\"]},"
        "{\"type\":\"T\",\"fields\":[\"n\",\"side\",\"entry_time\",\"exit_time\",\"entry_price\","
        "\"exit_price\",\"qty\",\"pnl\",\"pnl_pct\",\"max_runup\",\"max_drawdown\",\"commission\","
        "\"entry_bar_index\",\"exit_bar_index\",\"open_at_end\",\"entry_incarnation\","
        "\"close_cause\",\"entry_id\",\"exit_id\",\"exit_comment\"],"
        "\"types\":[\"i64\",\"str\",\"i64\",\"i64\",\"f64?\",\"f64?\",\"f64?\",\"f64?\",\"f64?\","
        "\"f64?\",\"f64?\",\"f64?\",\"i64\",\"i64\",\"bool\",\"u64\",\"i64\",\"str\",\"str\",\"str\"],"
        "\"units\":[\"\",\"\",\"unix_ms\",\"unix_ms\",\"quote_ccy\",\"quote_ccy\",\"qty\","
        "\"account_ccy\",\"percent\",\"account_ccy\",\"account_ccy\",\"account_ccy\","
        "\"script_bar\",\"script_bar\",\"\",\"\",\"\",\"\",\"\",\"\"]}]}\n";
}

// The zero long trade the rows write as T 1, with entry_incarnation 1 and
// close_cause 0: every f64 is 0.0, both bar indices 0, open_at_end false.
std::string trade_line(const std::string& entry_id) {
    return "[\"T\",1,\"long\",0,0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0,0,false,1,0,\"" + entry_id
        + "\",\"\",\"\"]\n";
}

std::string trailer_line(std::size_t rows, const std::string& digest) {
    return "{\"trailer\":\"pf-report-sink\",\"version\":1,\"counts\":{\"E\":" + std::to_string(rows)
        + ",\"T\":1},\"sha256\":\"" + digest + "\"}\n";
}

// Where the record bytes end relative to a window: exactly on a boundary,
// inside a window the trailer then fits, or so the trailer straddles one.
enum class Tail { exact, inside, straddle };

const char* tail_name(Tail tail) {
    return tail == Tail::exact ? "exact" : tail == Tail::inside ? "inside" : "straddle";
}

struct Population {
    std::vector<pf_equity_point_t> equity;
    std::string entry_id;
    std::vector<std::size_t> ends;  // end offset of the header, then of each record
    std::string expected;           // the complete stream, trailer included
    std::size_t prefix = 0;         // the bytes before the trailer line
    std::string digest;
};

// E row i: integer-valued equity prints as "<n>.0" and a signed zero keeps its
// sign, both exactly Python repr. The T row's entry id places the trailer.
Population make_population(std::size_t rows, Tail tail) {
    Population population;
    std::string stream = header_line();
    population.ends.push_back(stream.size());
    for (std::size_t i = 0; i < rows; ++i) {
        const auto whole = static_cast<long long>(10000 + i % 50000);
        const bool negative = i % 2 == 1;
        const pf_equity_point_t point{
            INT64_C(1700000000000) + static_cast<std::int64_t>(i) * INT64_C(60000),
            static_cast<double>(whole), negative ? -0.0 : 0.0};
        population.equity.push_back(point);
        stream += "[\"E\"," + std::to_string(static_cast<long long>(point.time_ms)) + ","
            + std::to_string(whole) + (negative ? ".0,-0.0]\n" : ".0,0.0]\n");
        population.ends.push_back(stream.size());
    }
    const auto trailer_bytes = trailer_line(rows, std::string(64, '0')).size();
    const std::size_t target = tail == Tail::exact ? 0
        : tail == Tail::inside ? kWindow / 3 : kWindow - trailer_bytes / 2;
    const auto before = stream.size() + trade_line("").size();
    population.entry_id.assign((target + kWindow - before % kWindow) % kWindow, 'x');
    stream += trade_line(population.entry_id);
    population.ends.push_back(stream.size());
    require(stream.size() % kWindow == target, "population records end at the wrong window offset");
    rs::Sha256 hash;
    hash.update(stream);
    population.digest = hash.hex_digest();
    population.prefix = stream.size();
    population.expected = stream + trailer_line(rows, population.digest);
    const bool crosses = population.prefix / kWindow != (population.expected.size() - 1) / kWindow;
    require(crosses == (tail == Tail::straddle), "population trailer placement differs from its tail");
    return population;
}

void write_records(rs::LineWriter& writer, const Population& population) {
    for (const auto& point : population.equity) writer.write_e(point);
    pf_trade_t trade{};
    trade.is_long = 1;
    writer.write_t(1, trade, 1, 0, population.entry_id, "", "");
}

void expect_trailer(const rs::Trailer& trailer, const Population& population, const std::string& row) {
    require(population.digest == trailer.sha256_hex
                && trailer.count_e == static_cast<std::int64_t>(population.equity.size())
                && trailer.count_t == 1 && trailer.count_o == 0 && trailer.count_v == 0
                && trailer.count_k == 0 && trailer.count_x == 0 && trailer.count_h == 0,
            row + ": trailer receipt differs from the known answer");
}

// ---- The window rule: the flushes the per-window writer already made ----

struct Flush { std::size_t offset, bytes, length; };

std::size_t aligned(std::size_t bytes) { return (bytes + kAlign - 1) / kAlign * kAlign; }

// Each flush is pwrite(offset, bytes) of the aligned window, then
// ftruncate(length) to the logical end: every window as it fills, the
// retained partial window (the trailer's first flush, or abandon), then, when
// a trailer follows (total > prefix), each window it completes and its final
// partial window.
std::vector<Flush> flushes(std::size_t prefix, std::size_t total) {
    std::vector<Flush> plan;
    const auto partial = [&plan](std::size_t end) {
        const auto base = end / kWindow * kWindow;
        if (end > base) plan.push_back({base, aligned(end - base), end});
    };
    for (std::size_t end = kWindow; end <= prefix; end += kWindow)
        plan.push_back({end - kWindow, kWindow, end});
    partial(prefix);
    if (total == prefix) return plan;
    for (std::size_t end = prefix / kWindow * kWindow + kWindow; end <= total; end += kWindow)
        plan.push_back({end - kWindow, kWindow, end});
    partial(total);
    return plan;
}

std::string describe(std::size_t at) {
    if (at >= probe.count) return "no further call";
    const auto& event = probe.events[at];
    return std::string(sinksync::kOpNames[slot(event.op)]) + "(" + std::to_string(event.position)
        + ", " + std::to_string(event.bytes) + ") = " + std::to_string(event.result);
}

std::size_t syncs(std::size_t from, std::size_t to) {
    std::size_t total = 0;
    for (std::size_t i = from; i < to; ++i)
        total += probe.events[i].op == Op::fdatasync || probe.events[i].op == Op::fsync;
    return total;
}

// The per-window writer's flush sequence without its per-flush syncs, then
// exactly one fdatasync, whose injected errno is `error` (0: success).
void expect_sequence(const std::string& row, std::size_t prefix, std::size_t total, int error) {
    require(!probe.overflow, row + ": the fixed I/O event log overflowed");
    const auto plan = flushes(prefix, total);
    std::size_t at = 0;
    for (std::size_t f = 0; f < plan.size(); ++f) {
        const auto& flush = plan[f];
        for (std::size_t done = 0; done < flush.bytes;) {
            const bool next = at < probe.count && probe.events[at].op == Op::pwrite
                && probe.events[at].position == static_cast<long long>(flush.offset + done)
                && probe.events[at].bytes == flush.bytes - done && probe.events[at].result > 0;
            require(next, row + ": call " + std::to_string(at) + " is " + describe(at) + "; flush "
                    + std::to_string(f) + " expects pwrite(" + std::to_string(flush.offset + done)
                    + ", " + std::to_string(flush.bytes - done) + ")");
            done += static_cast<std::size_t>(probe.events[at].result);
            ++at;
        }
        const bool truncated = at < probe.count && probe.events[at].op == Op::ftruncate
            && probe.events[at].position == static_cast<long long>(flush.length)
            && probe.events[at].result == 0;
        require(truncated, row + ": call " + std::to_string(at) + " is " + describe(at) + "; flush "
                + std::to_string(f) + " expects ftruncate(" + std::to_string(flush.length) + ")");
        ++at;
    }
    const bool synced = at < probe.count && probe.events[at].op == Op::fdatasync
        && probe.events[at].result == (error ? -1 : 0) && probe.events[at].error == error;
    require(synced, row + ": call " + std::to_string(at) + " is " + describe(at)
            + "; expected one fdatasync after the last of " + std::to_string(plan.size()) + " flushes");
    ++at;
    require(at == probe.count, row + ": " + std::to_string(probe.count - at)
            + " sink call(s) follow the deferred sync, first " + describe(at));
    require(probe.calls[slot(Op::fsync)] == 0, row + ": Linux synchronizes the sink with fdatasync only");
}

// ---- Files and failures ----

struct Scratch {
    std::string path;
    Scratch() {
        const char* temporary = std::getenv("TMPDIR");
        path = std::string(temporary && *temporary ? temporary : "/tmp") + "/pineforge-sinksync-XXXXXX";
        require(::mkdtemp(path.data()) != nullptr, "create the row directory");
    }
    ~Scratch() { ::rmdir(path.c_str()); }
};

// The prior public file survives failed/abandoned generations. The probe
// follows its private sibling, or this inode during unchanged-main replay.
struct PathSink {
    std::string directory, path;
    PathSink(const std::string& parent, const char* name)
        : directory(parent), path(parent + "/" + name) {
        Fd created(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
        struct stat info{};
        require(created.value >= 0 && ::fstat(created.value, &info) == 0, "create the sink file");
        sinksync::arm(info.st_dev, info.st_ino);
        require(::realpath(parent.c_str(), probe.directory.data()) != nullptr,
                "resolve the sink directory for private-file accounting");
        probe.directory_length = std::strlen(probe.directory.data());
    }
    ~PathSink() {
        probe.armed = false;
        ::unlink(path.c_str());
    }
    pf_report_sink_v1 descriptor() const {
        return {sizeof(pf_report_sink_v1), 1, path.c_str(), kRunId, directory.c_str()};
    }
};

std::string contents(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot read " + path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void expect_unpublished_path(const PathSink& sink) {
    require(contents(sink.path).empty(), "failure/abandon changed the previous public file");
    const std::filesystem::directory_iterator files(sink.directory);
    require(std::distance(files, std::filesystem::directory_iterator{}) == 1,
            "failure/abandon left an in-progress sibling");
}

std::size_t file_size(const std::string& path) {
    struct stat info{};
    require(::stat(path.c_str(), &info) == 0, "cannot stat " + path);
    return static_cast<std::size_t>(info.st_size);
}

// The bytes before a sink file's last line.
std::size_t record_prefix(const std::string& bytes) {
    require(bytes.size() > 1 && bytes.back() == '\n', "the sink file ends with LF");
    const auto cut = bytes.rfind('\n', bytes.size() - 2);
    require(cut != std::string::npos, "the sink file holds a header before its last line");
    return cut + 1;
}

std::string io_text(const char* operation, int number) {
    return std::string("report sink ") + operation + ": "
        + std::error_code(number, std::generic_category()).message();
}

void expect_writer_io(const rs::WriterError& error, int number, const char* operation) {
    require(error.reason() == rs::WriterFailure::io && error.error_number() == number,
            "writer I/O failure keeps its errno");
    const auto expected = io_text(operation, number);
    require(error.what() == expected,
            std::string("writer I/O failure text: ") + error.what() + ", expected " + expected);
    expect_failure(error.run_failure(), reason_args("sink_io"));
    expect_failure(pineforge::classify_run_failure(error), reason_args("sink_io"));
}

// A failed or abandoned writer publishes nothing and refuses a later trailer.
void expect_refused(rs::LineWriter& writer) {
    require(writer.diagnostics().complete == 0, "a failed sink published its diagnostics");
    bool refused = false;
    try {
        (void)writer.write_trailer();
    } catch (const rs::WriterError& error) {
        refused = error.reason() == rs::WriterFailure::invalid_state;
    }
    require(refused, "a failed sink accepted a later trailer");
}

// Reads a borrowed stream sink to EOF on its own thread, so the writer's
// poll() back-pressure never blocks the row.
class Drain {
public:
    Drain(Socket& socket, std::string& bytes)
        : socket_(socket), reader_([this, &bytes] { read_all(bytes); }) {}
    ~Drain() { finish(); }
    Drain(const Drain&) = delete;
    Drain& operator=(const Drain&) = delete;
    void finish() {
        socket_.writer.reset();
        if (reader_.joinable()) reader_.join();
    }

private:
    void read_all(std::string& bytes) {
        std::array<char, 65536> chunk{};
        for (;;) {
            const auto got = ::read(socket_.reader.value, chunk.data(), chunk.size());
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) return;
            bytes.append(chunk.data(), static_cast<std::size_t>(got));
        }
    }
    Socket& socket_;
    std::thread reader_;
};

class QuietHost final : public pineforge::NativeStrategyHost {
public:
    QuietHost() {
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
        require(configure_native(spec).status == pineforge::NativeSetupStatus::Applied,
                "configure the native sink host");
    }
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {}
};

// ---- Rows ----

// Conditions 1 and 5: unchanged pwrite/ftruncate sequence and bytes; the only
// sync is the last call, inside write_trailer.
void row_count(const std::string& directory) {
    for (const auto tail : {Tail::inside, Tail::straddle, Tail::exact}) {
        const std::string row = std::string("count ") + tail_name(tail);
        const auto population = make_population(kRows, tail);
        PathSink sink(directory, "count.sink");
        rs::LineWriter writer(sink.descriptor());
        writer.write_header(kRecords);
        write_records(writer, population);
        const auto before = probe.count;
        require(syncs(0, before) == 0, row + ": the writer synchronized " + std::to_string(syncs(0, before))
                + " time(s) before its trailer; expected none");
        const auto trailer = writer.write_trailer();
        expect_sequence(row, population.prefix, population.expected.size(), 0);
        expect_trailer(trailer, population, row);
        require(file_size(sink.path) == population.expected.size(), row + ": padding outlived the trailer");
        require(contents(sink.path) == population.expected, row + ": sink bytes differ from the known answer");
    }
}

// Conditions 1 and 3: the path route, the borrowed stream route and the
// independent known answer agree byte for byte; the borrowed route makes no
// positional write, truncation or sync anywhere in the process.
void row_bytes(const std::string& directory) {
    rs::Sha256 nist;
    nist.update("abc");
    require(nist.hex_digest() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "FIPS 180-2 B.1 SHA-256 known answer");
    for (const auto tail : {Tail::inside, Tail::straddle, Tail::exact}) {
        const std::string row = std::string("bytes ") + tail_name(tail);
        const auto population = make_population(kRows, tail);
        rs::Trailer path_trailer{};
        std::string path_bytes;
        {
            PathSink sink(directory, "bytes.sink");
            rs::LineWriter writer(sink.descriptor());
            writer.write_header(kRecords);
            write_records(writer, population);
            path_trailer = writer.write_trailer();
            path_bytes = contents(sink.path);
            require(file_size(sink.path) == population.expected.size(), row + ": path sink size");
        }
        require(path_bytes == population.expected, row + ": path sink bytes differ from the known answer");
        expect_trailer(path_trailer, population, row + " path");

        Socket socket;
        struct stat info{};
        require(::fstat(socket.writer.value, &info) == 0, "stat the stream sink");
        sinksync::arm(info.st_dev, info.st_ino);
        std::string received;
        rs::Trailer stream_trailer{};
        {
            Drain drain(socket, received);
            {
                const pf_report_sink_fd_v1 stream{sizeof stream, 1, socket.writer.value, kRunId};
                rs::LineWriter writer(stream);
                writer.write_header(kRecords);
                write_records(writer, population);
                stream_trailer = writer.write_trailer();
            }
            drain.finish();
        }
        probe.armed = false;
        for (std::size_t op = 0; op < probe.any.size(); ++op)
            require(probe.any[op] == 0, row + ": the borrowed route called " + sinksync::kOpNames[op]);
        require(received == population.expected, row + ": stream sink bytes differ from the known answer");
        expect_trailer(stream_trailer, population, row + " stream");
    }
}

// Condition 2: an injected EIO at the deferred sync fails write_trailer with
// the per-window sync's type and text, after every byte was written, and
// leaves no receipt.
void row_sync_fault(const std::string& directory) {
    const auto population = make_population(kRows, Tail::inside);
    PathSink sink(directory, "sync-fault.sink");
    sinksync::inject(Op::fdatasync, 1, EIO);
    rs::LineWriter writer(sink.descriptor());
    writer.write_header(kRecords);
    try {
        write_records(writer, population);
    } catch (const rs::WriterError& error) {
        throw std::runtime_error(std::string("a window sync failed a record before the trailer: ")
                                 + error.what());
    }
    bool caught = false;
    try {
        (void)writer.write_trailer();
    } catch (const rs::WriterError& error) {
        caught = true;
        expect_writer_io(error, EIO, "synchronize failed");
    }
    require(caught, "the failed deferred sync returned a trailer");
    expect_sequence("sync_fault", population.prefix, population.expected.size(), EIO);
    expect_refused(writer);
    expect_unpublished_path(sink);
}

// Condition 2: ENOSPC from the third window's pwrite surfaces at the record
// that fills that window, with the same type and text as before, and no sync
// ran before it.
void row_write_fault(const std::string& directory) {
    const auto population = make_population(kRows, Tail::inside);
    const std::size_t boundary = 3 * kWindow;
    std::size_t filler = 0;  // the line holding byte boundary - 1: 0 header, i E row i - 1
    while (filler < population.ends.size() && population.ends[filler] < boundary) ++filler;
    require(filler >= 1 && filler <= population.equity.size(), "the third window fills inside an E row");
    PathSink sink(directory, "write-fault.sink");
    sinksync::inject(Op::pwrite, 3, ENOSPC);
    rs::LineWriter writer(sink.descriptor());
    writer.write_header(kRecords);
    std::size_t failed = 0;
    for (std::size_t i = 0; i < population.equity.size() && failed == 0; ++i) {
        try {
            writer.write_e(population.equity[i]);
        } catch (const rs::WriterError& error) {
            failed = i + 1;
            expect_writer_io(error, ENOSPC, "write failed");
        }
    }
    require(failed == filler, "the third window's write failure surfaced at E row "
            + std::to_string(failed) + ", expected " + std::to_string(filler));
    require(probe.calls[slot(Op::pwrite)] == 3, "the failed write is the third window's pwrite");
    require(probe.calls[slot(Op::fdatasync)] == 0, "the writer synchronized "
            + std::to_string(probe.calls[slot(Op::fdatasync)])
            + " time(s) before the failed window write; expected none");
    expect_refused(writer);
    expect_unpublished_path(sink);
}

// Condition 2: EIO from the trailer's first truncation fails write_trailer
// with the same type and text as before, and no sync ran before it.
void row_truncate_fault(const std::string& directory) {
    const auto population = make_population(kRows, Tail::inside);
    const auto ordinal = population.prefix / kWindow + 1;
    PathSink sink(directory, "truncate-fault.sink");
    sinksync::inject(Op::ftruncate, ordinal, EIO);
    rs::LineWriter writer(sink.descriptor());
    writer.write_header(kRecords);
    write_records(writer, population);
    bool caught = false;
    try {
        (void)writer.write_trailer();
    } catch (const rs::WriterError& error) {
        caught = true;
        expect_writer_io(error, EIO, "truncate failed");
    }
    require(caught, "the failed trailer truncation returned a trailer");
    require(probe.calls[slot(Op::ftruncate)] == ordinal, "the failed truncation is the trailer's first");
    require(probe.calls[slot(Op::fdatasync)] == 0, "the writer synchronized "
            + std::to_string(probe.calls[slot(Op::fdatasync)])
            + " time(s) before the failed truncation; expected none");
    expect_refused(writer);
    expect_unpublished_path(sink);
}

// Conditions 1 and 6 on a real native run: windows reach the file during
// run(), the run makes no sync, and the report fill makes exactly one.
void row_engine(const std::string& directory) {
    QuietHost host;
    PathSink sink(directory, "engine.sink");
    install_path(host, sink.path, directory);
    run(host, tape(kEngineBars));
    require(host.last_run_status() == 0 && host.last_error().empty(),
            "the native sink run failed: " + host.last_error());
    const auto during = probe.count;
    require(syncs(0, during) == 0, "the run synchronized the sink " + std::to_string(syncs(0, during))
            + " time(s) before its report; expected none");
    std::size_t written = 0;
    for (std::size_t i = 0; i < during; ++i) written += probe.events[i].op == Op::pwrite;
    require(written >= 4, "the run wrote " + std::to_string(written)
            + " window(s) to the file; each filled window must reach it during the run");
    pineforge::ReportC report{};
    host.fill_report(&report);
    pineforge::BacktestEngine::free_report(&report);
    pf_report_sink_receipt_v1 receipt{sizeof receipt, 1};
    require(host.report_sink_receipt_v1(&receipt) == 0 && receipt.complete == 1,
            "the native sink completed its receipt");
    const auto bytes = contents(sink.path);
    const auto prefix = record_prefix(bytes);
    rs::Sha256 hash;
    hash.update(std::string_view(bytes).substr(0, prefix));
    require(hash.hex_digest() == receipt.sha256_hex, "the receipt digest differs from the sink records");
    require(bytes.compare(prefix, kTrailerStart.size(), kTrailerStart) == 0,
            "the native sink's last line is its trailer");
    expect_sequence("engine", prefix, bytes.size(), 0);
}

// Condition 2 on a real native run: the deferred sync fails inside the report
// fill (finish_report_sink), with report_sink_failed {"reason":"sink_io"} and
// the sync's text, and publishes no row, receipt or later report.
void row_engine_sync_fault(const std::string& directory) {
    QuietHost host;
    PathSink sink(directory, "engine-fault.sink");
    probe.capture_sync_bytes = true;
    sinksync::inject(Op::fdatasync, 1, EIO);
    install_path(host, sink.path, directory);
    run(host, tape(kEngineBars));
    require(host.last_run_status() == 0 && host.last_error().empty(),
            "the run failed before its report: " + host.last_error());
    const auto expected = io_text("synchronize failed", EIO);
    pineforge::ReportC report{};
    bool caught = false;
    try {
        host.fill_report(&report);
    } catch (const std::exception& error) {
        caught = true;
        require(error.what() == expected,
                std::string("report fill failure text: ") + error.what() + ", expected " + expected);
        expect_failure(pineforge::classify_run_failure(error), reason_args("sink_io"));
    }
    require(caught, "the failed deferred sync let the report fill succeed");
    require(report.trades == nullptr && report.trades_len == 0
                && report.equity_curve == nullptr && report.equity_curve_len == 0,
            "the failed sink left report rows");
    pineforge::BacktestEngine::free_report(&report);
    expect_engine_failure(host, reason_args("sink_io"));
    require(host.last_error().find(expected) != std::string::npos,
            "the latched failure text names the failed sync: " + host.last_error());
    require(probe.snapshot_ok, "capture the private file at the failed sync");
    const auto& bytes = probe.sync_bytes;
    expect_unpublished_path(sink);
    expect_sequence("engine_sync_fault", record_prefix(bytes), bytes.size(), EIO);
    pineforge::ReportC again{};
    host.fill_report(&again);
    require(again.trades == nullptr && again.equity_curve == nullptr && again.total_trades == 0,
            "a later report read published rows after the failed sink");
    pineforge::BacktestEngine::free_report(&again);
}

// Condition 5: abandon keeps its durability point. Its one sync covers every
// window, also when its own flush writes nothing.
void row_abandon(const std::string& directory) {
    for (const auto tail : {Tail::inside, Tail::exact}) {
        const std::string row = std::string("abandon ") + tail_name(tail);
        const auto population = make_population(kRows, tail);
        PathSink sink(directory, "abandon.sink");
        probe.capture_sync_bytes = true;
        rs::LineWriter writer(sink.descriptor());
        writer.write_header(kRecords);
        write_records(writer, population);
        const auto before = probe.count;
        require(syncs(0, before) == 0, row + ": the writer synchronized " + std::to_string(syncs(0, before))
                + " time(s) before abandon; expected none");
        writer.abandon();
        expect_sequence(row, population.prefix, population.prefix, 0);
        require(probe.snapshot_ok && probe.sync_bytes == population.expected.substr(0, population.prefix),
                row + ": the abandoned file is not the whole-record prefix");
        expect_unpublished_path(sink);
        expect_refused(writer);
    }
}

void row_close_fault(const std::string& directory) {
    const auto population = make_population(kRows, Tail::inside);
    PathSink sink(directory, "close-fault.sink");
    probe.close_after_sync = true;
    rs::LineWriter writer(sink.descriptor());
    writer.write_header(kRecords);
    write_records(writer, population);
    bool caught = false;
    try { (void)writer.write_trailer(); }
    catch (const rs::WriterError& error) {
        caught = true;
        expect_writer_io(error, EBADF, "close failed");
    }
    require(caught && probe.close_ok, "the real owned-close failure returned a trailer");
    expect_sequence("close_fault", population.prefix, population.expected.size(), 0);
    expect_unpublished_path(sink);
    expect_refused(writer);
}

struct Allocation { std::size_t calls, traffic, peak; };

struct Counting {
    Counting() {
        allocation_probe::calls = allocation_probe::traffic = allocation_probe::peak = 0;
        allocation_probe::retained = 0;
        allocation_probe::enabled = true;
    }
    ~Counting() { allocation_probe::enabled = false; }
};

Allocation allocations(const std::string& directory, const Population& population) {
    PathSink sink(directory, "memory.sink");
    const auto descriptor = sink.descriptor();
    {
        Counting counting;
        rs::LineWriter writer(descriptor);
        writer.write_header(kRecords);
        write_records(writer, population);
        (void)writer.write_trailer();
    }
    require(allocation_probe::retained == 0, "the writer retained heap after destruction");
    return {allocation_probe::calls, allocation_probe::traffic, allocation_probe::peak};
}

// Condition 6: the writer's heap traffic and peak do not depend on how many
// windows wait for the deferred sync.
void row_memory(const std::string& directory) {
    const auto small = make_population(2000, Tail::inside);
    const auto large = make_population(4 * kRows, Tail::inside);
    require(small.prefix < 2 * kWindow && large.prefix > 16 * kWindow,
            "memory populations span one and more than sixteen windows");
    const auto one = allocations(directory, small);
    const auto many = allocations(directory, large);
    std::printf("deferred sync heap: calls=%zu/%zu traffic=%zu/%zu peak=%zu/%zu\n",
                one.calls, many.calls, one.traffic, many.traffic, one.peak, many.peak);
    require(one.calls == many.calls && one.traffic == many.traffic && one.peak == many.peak,
            "writer heap traffic grows with the windows awaiting sync");
    require(many.peak <= 2 * 1024 * 1024, "writer heap exceeds its fixed line and window bound");
}

struct Umask {
    mode_t previous;
    explicit Umask(mode_t mask) : previous(::umask(mask)) {}
    ~Umask() { ::umask(previous); }
};

void row_permissions(const std::string& directory) {
    const auto population = make_population(kRows, Tail::inside);
    for (const mode_t mask : {0022, 0077, 0777}) {
        Umask mask_scope(mask);
        for (const bool existing : {false, true}) {
            PathSink sink(directory, "permissions.sink");
            // Ensure the reference target is 0600 even with umask 0777.
            require(::chmod(sink.path.c_str(), 0600) == 0, "set the reference target mode");
            if (!existing)
                require(::unlink(sink.path.c_str()) == 0, "prepare a new publication path");
            sinkmode::Watch watch(directory);
            rs::LineWriter writer(sink.descriptor());
            writer.write_header(kRecords);
            write_records(writer, population);
            require(sinkmode::probe.creations == 1 && sinkmode::probe.writes > 0,
                    "permission observation needs a real private file and written windows");
            const auto trailer = writer.write_trailer();
            expect_sequence("permissions", population.prefix, population.expected.size(), 0);
            expect_trailer(trailer, population, "permissions");
            require(contents(sink.path) == population.expected,
                    "permission handling changed the published bytes");
            struct stat info{};
            require(::stat(sink.path.c_str(), &info) == 0
                        && (info.st_mode & 0777) == 0600,
                    "replacement/new-file permissions must remain 0600");
            const auto& modes = sinkmode::probe;
            require(!modes.stat_failed && modes.observations > 0
                        && !modes.bad_creation && !modes.bad_write_mode
                        && modes.observed == 0600 && modes.chmods == 1,
                    "an in-progress report had permissions wider than 0600");
            require(std::distance(std::filesystem::directory_iterator(directory),
                                  std::filesystem::directory_iterator{}) == 1,
                    "permission row left an in-progress sibling");
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        Environment arena_fd("PF_SCRATCH_ARENA_FD", nullptr);
        Environment arena_bytes("PF_SCRATCH_ARENA_BYTES", nullptr);
        Environment window("PF_SCRATCH_WINDOW_BYTES", nullptr);
        const std::string selected = argc > 1 ? argv[1] : "all";
        const struct { const char* name; void (*row)(const std::string&); } rows[] = {
            {"count", row_count}, {"bytes", row_bytes}, {"sync_fault", row_sync_fault},
            {"write_fault", row_write_fault}, {"truncate_fault", row_truncate_fault},
            {"engine", row_engine}, {"engine_sync_fault", row_engine_sync_fault},
            {"abandon", row_abandon}, {"memory", row_memory},
            {"close_fault", row_close_fault}, {"permissions", row_permissions},
        };
        bool matched = false;
        for (const auto& entry : rows) {
            if (selected != "all" && selected != entry.name) continue;
            matched = true;
            Scratch directory;
            entry.row(directory.path);
            std::printf("report sink deferred sync: %s passed\n", entry.name);
        }
        require(matched, "unknown row " + selected);
        return 0;
    } catch (const std::exception& error) {
        allocation_probe::enabled = false;
        std::fprintf(stderr, "test_report_sink_deferred_sync: %s\n", error.what());
        return 1;
    }
}
