// Linux linker-wrap accounting of the real public JSON codec's two fragments.
// The strict Python reader supplies the bytes, counts and SHA-256 expectations.
// Every registered row must fail a sync assertion against the per-window codec.
#include <pineforge/report_sink_json.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr std::size_t window = 65536;
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

enum class Op { write, truncate, data_sync, full_sync };
struct Event {
    Op op{};
    off_t position = 0;
    std::size_t bytes = 0;
    ssize_t result = 0;
};
struct Target {
    dev_t device = 0;
    ino_t inode = 0;
    std::array<Event, 256> events{};
    std::size_t count = 0;
    bool overflow = false;
};
struct Probe {
    bool armed = false;
    std::array<Target, 2> targets{};
} probe;

void arm(int equity, int trades) {
    probe = Probe{};
    const int fds[] = {equity, trades};
    for (std::size_t i = 0; i < 2; ++i) {
        struct stat info{};
        require(::fstat(fds[i], &info) == 0, "stat fragment for accounting");
        probe.targets[i].device = info.st_dev;
        probe.targets[i].inode = info.st_ino;
    }
    probe.armed = true;
}
void note(int fd, Event event) {
    if (!probe.armed) return;
    struct stat info{};
    if (::fstat(fd, &info) != 0) return;
    for (auto& target : probe.targets) {
        if (info.st_dev != target.device || info.st_ino != target.inode) continue;
        if (target.count == target.events.size()) target.overflow = true;
        else target.events[target.count++] = event;
    }
}
} // namespace

extern "C" ssize_t __real_pwrite(int, const void*, std::size_t, off_t);
extern "C" int __real_ftruncate(int, off_t);
extern "C" int __real_fdatasync(int);
extern "C" int __real_fsync(int);

extern "C" ssize_t __wrap_pwrite(int fd, const void* data, std::size_t bytes, off_t offset) {
    const auto result = __real_pwrite(fd, data, bytes, offset);
    const int saved = errno;
    note(fd, {Op::write, offset, bytes, result});
    errno = saved;
    return result;
}
extern "C" int __wrap_ftruncate(int fd, off_t length) {
    const int result = __real_ftruncate(fd, length);
    const int saved = errno;
    note(fd, {Op::truncate, length, 0, result});
    errno = saved;
    return result;
}
extern "C" int __wrap_fdatasync(int fd) {
    const int result = __real_fdatasync(fd);
    const int saved = errno;
    note(fd, {Op::data_sync, 0, 0, result});
    errno = saved;
    return result;
}
extern "C" int __wrap_fsync(int fd) {
    const int result = __real_fsync(fd);
    const int saved = errno;
    note(fd, {Op::full_sync, 0, 0, result});
    errno = saved;
    return result;
}

namespace {
struct File {
    std::FILE* value;
    File() : value(std::tmpfile()) { require(value != nullptr, "tmpfile"); }
    explicit File(const std::string& path) : value(std::fopen(path.c_str(), "rb")) {
        require(value != nullptr, "open fixture " + path);
    }
    ~File() { std::fclose(value); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    int fd() const { return ::fileno(value); }
    std::string read() const {
        struct stat info{};
        require(::fstat(fd(), &info) == 0 && info.st_size >= 0, "stat fragment");
        std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
        std::size_t at = 0;
        while (at < bytes.size()) {
            const auto got = ::pread(fd(), bytes.data() + at, bytes.size() - at,
                                     static_cast<off_t>(at));
            if (got < 0 && errno == EINTR) continue;
            require(got > 0, "read fragment");
            at += static_cast<std::size_t>(got);
        }
        return bytes;
    }
};
std::string contents(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "open expectation " + path);
    std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    require(!input.bad(), "read expectation " + path);
    return bytes;
}
struct Fixture {
    std::string directory;
    std::string header, equity, trades, offending;
    std::string source_digest, equity_digest, trades_digest;
    std::int64_t count_e = 0, count_t = 0;
    std::uint64_t bad_line = 0, bad_offset = 0;
    explicit Fixture(const std::string& path)
        : directory(path), header(contents(path + "/header.expected")),
          equity(contents(path + "/equity.expected")), trades(contents(path + "/trades.expected")),
          offending(contents(path + "/offending.expected")) {
        std::ifstream metadata(path + "/metadata.expected");
        require(bool(metadata >> count_e >> count_t >> source_digest >> equity_digest
                              >> trades_digest >> bad_line >> bad_offset), "strict-reader metadata");
        require(count_e >= 0 && count_t >= 0, "nonnegative fixture counts");
        require(source_digest.size() == 64 && equity_digest.size() == 64 && trades_digest.size() == 64,
                "strict-reader digest lengths");
    }
};
struct Call {
    std::array<char, PF_REPORT_SINK_JSON_BUFFER_BYTES> header{};
    std::array<std::uint8_t, PF_REPORT_SINK_JSON_BUFFER_BYTES> offending{};
    pf_report_sink_json_request_v1 request{};
    pf_report_sink_json_result_v1 result{};
    Call(int source, int equity, int trades) {
        request.struct_size = sizeof request;
        request.version = 1;
        request.operation = PF_SINK_JSON_PREPARE;
        request.sink_fd = source;
        request.equity_fd = equity;
        request.trades_fd = trades;
        request.header_buffer = header.data();
        request.header_capacity = header.size();
        request.error_buffer = offending.data();
        request.error_capacity = offending.size();
        result.struct_size = sizeof result;
        result.version = 1;
    }
    void run() {
        const int returned = pf_report_sink_json_v1(&request, &result);
        probe.armed = false;
        require(returned == 0, "public codec refused a valid descriptor");
    }
};

void counts_and_header(const Call& call, const Fixture& fixture) {
    require(call.result.counts[0] == fixture.count_e && call.result.counts[1] == fixture.count_t,
            "E/T counts differ from the strict reader");
    for (std::size_t i = 2; i < 7; ++i)
        require(call.result.counts[i] == 0, "unexpected unselected tag count");
    require(call.result.header_bytes == fixture.header.size()
            && std::memcmp(call.header.data(), fixture.header.data(), fixture.header.size()) == 0,
            "header bytes differ from the strict reader");
}
void sequence(const Target& target, std::size_t logical, bool finished, const std::string& tag) {
    require(!target.overflow, tag + ": accounting overflow");
    const auto windows = logical / window + (finished && logical % window ? 1 : 0);
    const auto syncs = finished && logical ? 1u : 0u;
    std::size_t actual_syncs = 0;
    for (std::size_t i = 0; i < target.count; ++i)
        actual_syncs += target.events[i].op == Op::data_sync || target.events[i].op == Op::full_sync;
    require(actual_syncs == syncs, tag + ": sync count " + std::to_string(actual_syncs)
            + ", expected " + std::to_string(syncs));
    require(target.count == 2 * windows + syncs, tag + ": write/truncate/sync event count");
    for (std::size_t i = 0; i < windows; ++i) {
        const auto used = std::min(window, logical - i * window);
        const auto aligned = (used + 4095) & ~std::size_t(4095);
        const auto& write = target.events[2 * i];
        const auto& truncate = target.events[2 * i + 1];
        require(write.op == Op::write && write.position == static_cast<off_t>(i * window)
                && write.bytes == aligned && write.result == static_cast<ssize_t>(aligned),
                tag + ": positional window write changed");
        require(truncate.op == Op::truncate && truncate.position == static_cast<off_t>(i * window + used)
                && truncate.result == 0, tag + ": window truncation changed");
    }
    if (syncs) {
        const auto& last = target.events[target.count - 1];
        require(last.op == Op::data_sync && last.result == 0,
                tag + ": final operation must be the single successful fdatasync");
    }
}
void accepted(const Fixture& fixture, bool deferred = false, bool outputs = true) {
    File source(fixture.directory + "/source.sink"), equity, trades;
    require(::lseek(source.fd(), 3, SEEK_SET) == 3
            && ::lseek(equity.fd(), 7, SEEK_SET) == 7
            && ::lseek(trades.fd(), 11, SEEK_SET) == 11, "set borrowed fd offsets");
    auto call = std::make_unique<Call>(source.fd(), outputs ? equity.fd() : -1,
                                      outputs ? trades.fd() : -1);
    arm(equity.fd(), trades.fd());
    call->run();
    const auto& result = call->result;
    require(result.status == PF_SINK_JSON_OK && result.complete == 1, "valid sink incomplete");
    require(result.summary.complete == 1 && result.diagnostics.complete == 1,
            "fold completion changed");
    require(result.summary.count_t == fixture.count_t && result.diagnostics.count_e == fixture.count_e,
            "fold counts differ from the strict reader");
    require(result.result_tag == (deferred ? static_cast<std::uint32_t>('E') : 0)
            && result.result_field == (deferred ? 2u : 0u), "deferred result witness changed");
    counts_and_header(*call, fixture);
    require(result.sha256_hex == fixture.source_digest, "source SHA-256 differs from the strict reader");
    require(result.equity_json_bytes == fixture.equity.size()
            && result.trades_json_bytes == fixture.trades.size(), "fragment lengths changed");
    require(result.equity_json_sha256_hex == fixture.equity_digest
            && result.trades_json_sha256_hex == fixture.trades_digest,
            "fragment SHA-256 differs from Python hashlib");
    require(equity.read() == (outputs ? fixture.equity : std::string{})
            && trades.read() == (outputs ? fixture.trades : std::string{}),
            "fragment bytes differ from the strict reader");
    require(::lseek(source.fd(), 0, SEEK_CUR) == 3
            && ::lseek(equity.fd(), 0, SEEK_CUR) == 7
            && ::lseek(trades.fd(), 0, SEEK_CUR) == 11, "borrowed fd offsets changed");
    sequence(probe.targets[0], outputs ? fixture.equity.size() : 0, true, "E");
    sequence(probe.targets[1], outputs ? fixture.trades.size() : 0, true, "T");
}
void discarded(const Fixture& fixture, bool unsupported) {
    File source(fixture.directory + "/source.sink"), equity, trades;
    auto call = std::make_unique<Call>(source.fd(), equity.fd(), trades.fd());
    arm(equity.fd(), trades.fd());
    call->run();
    const auto& result = call->result;
    require(result.status == (unsupported ? PF_SINK_JSON_NOT_IMPLEMENTED : PF_SINK_JSON_SOURCE_INVALID)
            && result.complete == 0, "late refusal class changed");
    require(result.error.stage == (unsupported ? PF_SINK_JSON_PREPARE_RECORD : PF_SINK_JSON_PREPARE_TRAILER)
            && result.error.category == (unsupported ? PF_SINK_JSON_UNSUPPORTED : PF_SINK_JSON_TRAILER),
            "late refusal stage/category changed");
    require(std::strcmp(result.error.message, unsupported ? "record or JSON shape not implemented"
                                                        : "trailer count mismatch") == 0,
            "late refusal text changed");
    require(result.error.line_number == fixture.bad_line && result.error.byte_offset == fixture.bad_offset
            && result.error.os_errno == 0, "late refusal location/errno changed");
    require(result.error_bytes == fixture.offending.size()
            && std::memcmp(call->offending.data(), fixture.offending.data(), fixture.offending.size()) == 0,
            "late refusal did not retain the actual offending bytes");
    counts_and_header(*call, fixture);
    require(result.equity_json_bytes == 0 && result.trades_json_bytes == 0
            && result.equity_json_sha256_hex[0] == '\0' && result.trades_json_sha256_hex[0] == '\0',
            "a refused source published finalized fragment metadata");
    const auto written_e = fixture.equity.size() / window * window;
    const auto written_t = fixture.trades.size() / window * window;
    require(written_e >= 5 * window && written_t >= 5 * window,
            "refusal must follow at least five written windows in each fragment");
    require(equity.read() == fixture.equity.substr(0, written_e)
            && trades.read() == fixture.trades.substr(0, written_t),
            "discarded fragment prefix bytes changed");
    sequence(probe.targets[0], fixture.equity.size(), false, "discarded E");
    sequence(probe.targets[1], fixture.trades.size(), false, "discarded T");
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3, "expected a row name and a strict-reader fixture directory");
        const std::string mode = argv[1];
        const Fixture fixture(argv[2]);
        require(fixture.equity.size() >= 5 * window && fixture.trades.size() >= 5 * window,
                "both fragments must span at least five windows");
        if (mode == "source_refusal" || mode == "unsupported") {
            discarded(fixture, mode == "unsupported");
        } else {
            require(mode == "partial" || mode == "exact_equity" || mode == "exact_trades"
                    || mode == "deferred_result", "unknown row " + mode);
            if (mode == "exact_equity")
                require(fixture.equity.size() == 6 * window, "E must end on an exact window boundary");
            if (mode == "exact_trades")
                require(fixture.trades.size() == 6 * window, "T must end on an exact window boundary");
            accepted(fixture, mode == "deferred_result");
            if (mode == "partial") {
                const Fixture empty(fixture.directory + "/empty");
                require(empty.count_e == 0 && empty.count_t == 0
                        && empty.equity.empty() && empty.trades.empty(), "empty fixture is not empty");
                accepted(empty);
                const Fixture equity_only(fixture.directory + "/equity_only");
                require(equity_only.equity.size() >= 5 * window && equity_only.trades.empty(),
                        "E-only fixture must have one nonempty fragment");
                accepted(equity_only);
                const Fixture trades_only(fixture.directory + "/trades_only");
                require(trades_only.trades.size() >= 5 * window && trades_only.equity.empty(),
                        "T-only fixture must have one nonempty fragment");
                accepted(trades_only);
                accepted(fixture, false, false);
            }
        }
        std::printf("report sink JSON deferred sync: %s passed\n", mode.c_str());
        return 0;
    } catch (const std::exception& error) {
        probe.armed = false;
        std::fprintf(stderr, "report sink JSON deferred sync: %s\n", error.what());
        return 1;
    }
}
