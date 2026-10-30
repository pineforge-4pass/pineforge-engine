// Physical Linux syscall accounting over the real no-history sink fixture.
#if !defined(__linux__)
#error "Physical ledger accounting requires Linux linker wrapping"
#endif
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "report_sink_trade_retirement_fixture.hpp"
#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/version.h>
#include "../src/sha256.hpp"

#include <iterator>
#include <string_view>
#include <sys/uio.h>

extern "C" __attribute__((visibility("default")))
int strategy_reads_trade_history() { return 0; }

namespace stream_io {
using Count = std::uint64_t;
enum class File : std::size_t { index, payload, other };
struct Io {
    Count writes = 0, written = 0, reads = 0, read = 0, truncates = 0, alternate = 0;
    int fd = -1;
    dev_t device = 0;
    ino_t inode = 0;
};
struct Counters {
    std::array<Io, 2> files{};
    bool classification_error = false, transfer_error = false;
};
bool measuring = false;
Counters counters;

// Classify each actual descriptor anew, so reuse cannot inherit a file class.
File identify(int fd) noexcept {
    char path[64]{}, name[4096]{};
    struct stat status{};
    const int length = std::snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof path
        || ::fstat(fd, &status) != 0) {
        counters.classification_error = true;
        return File::other;
    }
    const auto used = ::readlink(path, name, sizeof name - 1);
    if (used < 0 || static_cast<std::size_t>(used) == sizeof name - 1) {
        counters.classification_error = true;
        return File::other;
    }
    name[used] = '\0';
    const char* slash = std::strrchr(name, '/');
    const char* base = slash ? slash + 1 : name;
    constexpr const char* prefixes[]{"pineforge-trade-index", "pineforge-trade-payload"};
    for (std::size_t i = 0; i < std::size(prefixes); ++i) {
        const auto bytes = std::strlen(prefixes[i]);
        if (std::strncmp(base, prefixes[i], bytes) != 0 || base[bytes] != '-') continue;
        auto& io = counters.files[i];
        if (!S_ISREG(status.st_mode)
            || (io.fd >= 0 && (io.device != status.st_dev || io.inode != status.st_ino)))
            counters.classification_error = true;
        io.fd = fd;
        io.device = status.st_dev;
        io.inode = status.st_ino;
        return static_cast<File>(i);
    }
    return File::other;
}
void transfer(int fd, std::size_t bytes, ssize_t result, bool write) noexcept {
    if (!measuring) return;
    const int saved = errno;
    const auto file = identify(fd);
    if (file != File::other) {
        auto& io = counters.files[static_cast<std::size_t>(file)];
        ++(write ? io.writes : io.reads);
        if (result >= 0) (write ? io.written : io.read) += static_cast<Count>(result);
        counters.transfer_error |= result < 0 || static_cast<std::size_t>(result) != bytes;
    }
    errno = saved;
}
void other(int fd, bool truncate = false, int result = 0) noexcept {
    if (!measuring || fd < 0) return;
    const int saved = errno;
    const auto file = identify(fd);
    if (file != File::other) {
        auto& io = counters.files[static_cast<std::size_t>(file)];
        ++(truncate ? io.truncates : io.alternate);
        if (truncate && result != 0) counters.transfer_error = true;
    }
    errno = saved;
}
struct Measure {
    Measure() {
        trade_retirement_fixture::require(!measuring, "nested physical I/O measurement");
        counters = {};
        measuring = true;
    }
    ~Measure() { measuring = false; }
    Counters finish() { measuring = false; return counters; }
};
} // namespace stream_io

extern "C" ssize_t __real_pwrite(int, const void*, std::size_t, off_t);
extern "C" ssize_t __real_pread(int, void*, std::size_t, off_t);
extern "C" int __real_ftruncate(int, off_t);
extern "C" ssize_t __real_write(int, const void*, std::size_t);
extern "C" ssize_t __real_read(int, void*, std::size_t);
extern "C" ssize_t __real_writev(int, const struct iovec*, int);
extern "C" ssize_t __real_readv(int, const struct iovec*, int);
extern "C" ssize_t __real_pwritev(int, const struct iovec*, int, off_t);
extern "C" ssize_t __real_preadv(int, const struct iovec*, int, off_t);
extern "C" void* __real_mmap(void*, std::size_t, int, int, int, off_t);

extern "C" ssize_t __wrap_pwrite(int fd, const void* data, std::size_t bytes, off_t offset) {
    const auto result = __real_pwrite(fd, data, bytes, offset);
    stream_io::transfer(fd, bytes, result, true);
    return result;
}
extern "C" ssize_t __wrap_pread(int fd, void* data, std::size_t bytes, off_t offset) {
    const auto result = __real_pread(fd, data, bytes, offset);
    stream_io::transfer(fd, bytes, result, false);
    return result;
}
extern "C" int __wrap_ftruncate(int fd, off_t bytes) {
    const auto result = __real_ftruncate(fd, bytes);
    stream_io::other(fd, true, result);
    return result;
}
extern "C" ssize_t __wrap_write(int fd, const void* data, std::size_t bytes) {
    const auto result = __real_write(fd, data, bytes);
    stream_io::other(fd);
    return result;
}
extern "C" ssize_t __wrap_read(int fd, void* data, std::size_t bytes) {
    const auto result = __real_read(fd, data, bytes);
    stream_io::other(fd);
    return result;
}
extern "C" ssize_t __wrap_writev(int fd, const struct iovec* data, int count) {
    const auto result = __real_writev(fd, data, count);
    stream_io::other(fd);
    return result;
}
extern "C" ssize_t __wrap_readv(int fd, const struct iovec* data, int count) {
    const auto result = __real_readv(fd, data, count);
    stream_io::other(fd);
    return result;
}
extern "C" ssize_t __wrap_pwritev(int fd, const struct iovec* data, int count, off_t offset) {
    const auto result = __real_pwritev(fd, data, count, offset);
    stream_io::other(fd);
    return result;
}
extern "C" ssize_t __wrap_preadv(int fd, const struct iovec* data, int count, off_t offset) {
    const auto result = __real_preadv(fd, data, count, offset);
    stream_io::other(fd);
    return result;
}
extern "C" void* __wrap_mmap(void* address, std::size_t bytes, int protection,
                            int flags, int fd, off_t offset) {
    auto* result = __real_mmap(address, bytes, protection, flags, fd, offset);
    if ((flags & MAP_ANONYMOUS) == 0) stream_io::other(fd);
    return result;
}

namespace {
using namespace pineforge;
using trade_retirement_fixture::require;
using stream_io::Count;
constexpr Count kClosed = 32 * TradeLedger::kResidentRows;
constexpr Count kBudget = detail::ScratchFile::kCacheBytes;
constexpr char kRunId[] = "closed-trade-stream";
static_assert(kClosed >= 2000);
int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL %s\n", message); }
}
void bound(const char* metric, Count actual, Count allowed, Count trades) {
    const bool ok = actual <= allowed;
    std::printf("%s total=%llu per_closed_trade=%.9Lf allowed=%llu T=%llu %s\n",
        metric, static_cast<unsigned long long>(actual),
        static_cast<long double>(actual) / trades, static_cast<unsigned long long>(allowed),
        static_cast<unsigned long long>(trades), ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}
void valid(const stream_io::Counters& io) {
    require(!io.classification_error && !io.transfer_error,
            "physical ledger classification or full-transfer precondition failed");
    for (const auto& file : io.files)
        require(file.fd >= 0 && file.alternate == 0, "missing ledger file or alternate I/O");
}
struct Directory {
    std::string path;
    Directory() {
        std::string refusal;
        int error = 0;
        path = detail::resolve_scratch_directory("", refusal, error);
        require(!path.empty(), "cannot select disk-backed scratch directory");
        path += "/pineforge-closed-trade-stream-XXXXXX";
        require(::mkdtemp(path.data()) != nullptr, "cannot create fixture directory");
    }
    ~Directory() {
        ::unlink((path + "/report.ndjson").c_str());
        ::rmdir(path.c_str());
    }
};

// Independent v1 schema, with hash recording disabled so only E/T are enabled.
std::string header() {
    return std::string("{\"schema\":\"pf-report-sink\",\"version\":1,\"run_id\":\"") + kRunId
        + "\",\"engine\":\"" + PINEFORGE_VERSION_STRING + "\",\"records\":["
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
std::int64_t open_time(Count bar) {
    return trade_retirement_fixture::start_ms
        + static_cast<std::int64_t>(bar) * trade_retirement_fixture::minute;
}
std::string trade_line(Count index, bool terminal = false) {
    // A unit reversal fills at each script close. Row i entered on bar i
    // and exits on bar i+1; both times are the bars' open labels (lab-observed
    // on main: entry_time 1704067200000 = bar 0, exit_time = bar 1 for row 0).
    // The terminal lot is marked at the final open label.
    const Count exit_bar = terminal ? index : index + 1;
    const auto entry_time = open_time(index);
    const auto exit_time = open_time(exit_bar);
    return "[\"T\"," + std::to_string(index + 1)
        + (index % 2 == 0 ? ",\"long\"," : ",\"short\",")
        + std::to_string(entry_time) + "," + std::to_string(exit_time)
        + ",100.0,100.0,1.0,0.0,0.0,0.0,0.0,0.0,"
        + std::to_string(index) + "," + std::to_string(exit_bar)
        + (terminal ? ",true," : ",false,") + std::to_string(index + 1) + ","
        + std::to_string(static_cast<int>(terminal ? execution::CloseCause::RangeEnd
                                                  : execution::CloseCause::Script))
        + ",\"flip\"," + (terminal ? "\"\"" : "\"flip\"") + ",\"\"]\n";
}
struct Geometry {
    Count trades = 0, extents = 0, text_bytes = 0, retired_bytes = 0;
    Count retired_extents = 0, max_text_bytes = 0;
};
void observe(std::string_view line, Count retired, Geometry& geometry) {
    if (line.substr(0, 5) != "[\"T\",") return;
    std::array<std::string_view, 21> fields{};
    std::size_t start = 0;
    for (auto& field : fields) {
        const auto stop = line.find(',', start);
        field = line.substr(start, stop == std::string_view::npos ? stop : stop - start);
        start = stop == std::string_view::npos ? line.size() : stop + 1;
    }
    if (fields[15] == "true") return; // Report-only mark never enters the ledger.
    require(fields[15] == "false" && fields[18] == "\"flip\""
            && fields[19] == "\"flip\"" && fields[20] == "\"\"]",
            "produced row does not have the fixture's two IDs and empty comments");
    const auto number = static_cast<Count>(std::stoull(std::string(fields[1])));
    require(number == geometry.trades + 1, "produced closed rows are not contiguous");
    Count bytes = 0, extents = 0;
    for (auto field : {fields[18], fields[19]}) {
        require(field.size() >= 2 && field.front() == '"' && field.back() == '"',
                "unexpected escaped ID in fixed fixture");
        const Count length = field.size() - 2;
        bytes += length;
        extents += length != 0;
    }
    ++geometry.trades;
    geometry.extents += extents;
    geometry.text_bytes += bytes;
    geometry.max_text_bytes = std::max(geometry.max_text_bytes, bytes);
    if (number <= retired) {
        geometry.retired_bytes += bytes;
        geometry.retired_extents += extents;
    }
}
Geometry golden(const std::string& path, Count retired,
                const pf_report_sink_receipt_v1& receipt) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "cannot read completed report stream");
    report_sink::Sha256 hash;
    Geometry geometry;
    Count line_number = 0;
    bool identical = true;
    const auto expect = [&](const std::string& expected, bool hashed = true) {
        std::string actual;
        require(bool(std::getline(input, actual)), "report stream ended before golden");
        require(!input.eof(), "report stream lost its final newline");
        observe(actual, retired, geometry);
        actual += '\n';
        ++line_number;
        if (actual != expected && identical) {
            std::fprintf(stderr, "report first_difference_line=%llu\nexpected: %sactual: %s",
                static_cast<unsigned long long>(line_number), expected.c_str(), actual.c_str());
            identical = false;
        }
        if (hashed) hash.update(expected);
    };
    expect(header());
    // Every line's content is computed here from the scenario's inputs. The
    // interleaving is the engine's published stream order, observed on main
    // (v1.9.0 behaviour; the byte-identity rows compare head and main): one E
    // line per bar, and trade i (0-based, exiting at bar i + 2) streams right
    // after the E line of that bar while it is published (i < retired); the
    // rows still held back when the run ends follow the last E line.
    for (Count bar = 0; bar <= kClosed; ++bar) {
        expect("[\"E\"," + std::to_string(open_time(bar)) + ",1000000.0,0.0]\n");
        if (bar >= 2 && bar - 2 < retired) expect(trade_line(bar - 2));
    }
    for (Count i = retired; i < kClosed; ++i) expect(trade_line(i));
    expect(trade_line(kClosed, true));
    const auto digest = hash.hex_digest();
    expect("{\"trailer\":\"pf-report-sink\",\"version\":1,\"counts\":{\"E\":"
        + std::to_string(kClosed + 1) + ",\"T\":" + std::to_string(kClosed + 1)
        + "},\"sha256\":\"" + digest + "\"}\n", false);
    check(identical, "complete report bytes differ from independent golden");
    check(input.peek() == std::char_traits<char>::eof(), "extra bytes after report trailer");
    check(receipt.complete == 1 && receipt.trades_retained == 0
          && receipt.count_e == static_cast<std::int64_t>(kClosed + 1)
          && receipt.count_t == static_cast<std::int64_t>(kClosed + 1)
          && receipt.sha256_hex == digest, "report receipt differs from golden");
    return geometry;
}
void raw_zeros(const stream_io::Io& file, Count first, Count stop) {
    struct stat status{};
    require(::fstat(file.fd, &status) == 0 && status.st_dev == file.device
            && status.st_ino == file.inode && static_cast<Count>(status.st_size) >= stop,
            "raw ledger inspection lost its backing extent");
    std::array<unsigned char, 4096> bytes{};
    while (first < stop) {
        const auto count = static_cast<std::size_t>(std::min<Count>(bytes.size(), stop - first));
        // Inspection deliberately bypasses measurement; these are not engine reads.
        require(__real_pread(file.fd, bytes.data(), count, static_cast<off_t>(first))
                    == static_cast<ssize_t>(count), "raw ledger prefix read failed");
        check(std::all_of(bytes.begin(), bytes.begin() + count,
                          [](unsigned char byte) { return byte == 0; }),
              "retired scratch prefix contains nonzero bytes");
        first += count;
    }
}
void forwarding_control(const Directory& directory) {
    detail::ScratchFile index, payload;
    index.set_directory(directory.path);
    payload.set_directory(directory.path);
    index.open("pineforge-trade-index");
    payload.open("pineforge-trade-payload");
    constexpr char text[] = "flip";
    const std::array<unsigned char, TradeLedger::kRecordBytes> descriptor{};
    auto copied = descriptor;
    std::array<char, sizeof text - 1> copy{};
    stream_io::Measure measurement;
    index.write(0, descriptor.data(), descriptor.size());
    payload.write(0, text, copy.size());
    index.read(0, copied.data(), copied.size());
    payload.read(0, copy.data(), copy.size());
    index.truncate(0);
    payload.truncate(0);
    const auto io = measurement.finish();
    valid(io);
    for (const auto& file : io.files)
        require(file.writes == 1 && file.reads == 1 && file.truncates == 1,
                "real syscall forwarding control did not count each operation");
    require(io.files[0].written == descriptor.size() && io.files[0].read == descriptor.size()
            && io.files[1].written == copy.size() && io.files[1].read == copy.size()
            && copied == descriptor && std::memcmp(copy.data(), text, copy.size()) == 0,
            "real syscall forwarding control changed bytes");
    report_sink::Sha256 known;
    known.update("abc");
    require(known.hex_digest()
                == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "independent hash known-answer control failed");
}
void scenario(const Directory& directory) {
    require(!detail::ScratchArena::attached(), "physical fixture inherited an arena");
    trade_retirement_fixture::Strategy host(false, false, false, nullptr);
    host.set_broker_state_hash_recording(false);
    const std::string path = directory.path + "/report.ndjson";
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), kRunId, directory.path.c_str()};
    require(host.set_report_sink_v1(&sink) == 0, "no-history report sink refused");
    std::vector<Bar> bars(kClosed + 1);
    for (Count i = 0; i < bars.size(); ++i) bars[i] = {100, 100, 100, 100, 1, open_time(i)};
    stream_io::Measure measurement;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    const auto io = measurement.finish();
    require(host.last_run_status() == 0 && host.last_error().empty(), "real source run failed");
    require(host.callbacks == bars.size() && host.closes == kClosed
            && host.closed_trade_count() == kClosed, "real fixture lost reversal trades");
    // Before the final script callback there are N-1 closes. Keeping the
    // latest exit block and its predecessor leaves R=(N-1)-2=N-3 published.
    const Count retired = host.base();
    require(retired == kClosed - 3, "one-trade-per-bar publication geometry changed");
    valid(io);
    ReportC report{};
    host.fill_report(&report);
    const bool report_ok = report.total_trades == static_cast<int>(kClosed + 1)
        && report.net_profit == 0 && report.trades == nullptr && report.trades_len == 0;
    BacktestEngine::free_report(&report);
    require(report_ok, "no-history report materialized rows or changed zero-PnL totals");
    pf_report_sink_receipt_v1 receipt{sizeof receipt, 1};
    require(host.report_sink_receipt_v1(&receipt) == 0, "sink receipt refused");
    const auto geometry = golden(path, retired, receipt);
    require(geometry.trades == kClosed, "produced report lost closed rows");

    const auto& index = io.files[0];
    const auto& payload = io.files[1];
    const Count descriptor_bytes = geometry.trades * TradeLedger::kRecordBytes;
    const Count index_dead = retired * TradeLedger::kRecordBytes;
    const Count index_bytes = descriptor_bytes + index_dead;
    const Count payload_bytes = geometry.text_bytes + geometry.retired_bytes;
    // Below one release chunk: each single-row seal adds one head-zero request
    // per file. No empty truncation or copy occurs while the two-row suffix lives.
    require(descriptor_bytes < detail::ScratchArena::kChunkBytes
            && geometry.text_bytes < detail::ScratchArena::kChunkBytes
            && geometry.max_text_bytes <= 4096, "fixture crossed its no-compaction geometry");
    // For ordered positive requests l_j, A_j=sum earlier l:
    // calls_j=1+floor((A_j+l_j-1)/W)-floor(A_j/W).
    // Summing permits at most floor(total_bytes/W) extra splits per file.
    // MUTANT: restore erase_payload(text) in retire: adds retired_extents
    // payload requests and retired_bytes bytes, exceeding the payload bounds.
    // Duplicating store's index.write breaks both index write bounds.
    bound("stream.index.pwrite_calls", index.writes,
          geometry.trades + retired + index_bytes / kBudget, geometry.trades);
    bound("stream.payload.pwrite_calls", payload.writes,
          geometry.extents + retired + payload_bytes / kBudget, geometry.trades);
    bound("stream.index.pwrite_bytes", index.written, index_bytes, geometry.trades);
    bound("stream.payload.pwrite_bytes", payload.written, payload_bytes, geometry.trades);
    require(index.written >= descriptor_bytes && payload.written >= geometry.text_bytes,
            "physical accounting omitted appended row storage");
    // At most three live rows and one newly closed row; all descriptors fit
    // the 64-row ring, all live text fits the fixed 64*168-byte payload tail.
    // cold=max(0,live_rows-capacity)=0; no reads or compaction truncates.
    // MUTANTS: read index in descriptor's resident branch, or read payload
    // in read_string's tail branch: each breaks its respective zero-read bound.
    const Count live = geometry.trades - retired + 1;
    const Count cold_index = live - std::min<Count>(live, TradeLedger::kResidentRows);
    const Count tail_rows = TradeLedger::kResidentRows * TradeLedger::kRecordBytes
        / geometry.max_text_bytes;
    const Count cold_payload = live - std::min(live, tail_rows);
    bound("stream.index.pread_calls", index.reads, cold_index, geometry.trades);
    bound("stream.payload.pread_calls", payload.reads, cold_payload, geometry.trades);
    const Count index_releases = index_dead / detail::ScratchArena::kChunkBytes;
    const Count payload_releases = geometry.retired_bytes / detail::ScratchArena::kChunkBytes;
    // MUTANT: truncate each file to its existing storage_bytes() in
    // release_head even when no chunk is released: breaks its truncate bound.
    bound("stream.index.ftruncate_calls", index.truncates, index_releases, geometry.trades);
    bound("stream.payload.ftruncate_calls", payload.truncates, payload_releases, geometry.trades);
    raw_zeros(index, 0, index_dead);
    raw_zeros(payload, 0, geometry.retired_bytes);
    // Refusals must precede every backing access, even an empty comparison.
    stream_io::Measure refusal_measurement;
    trade_retirement_fixture::refused([&] { (void)host.closed_trade(0); });
    trade_retirement_fixture::refused([&] { (void)host.old_entry_id_equals(""); });
    const auto refused = refusal_measurement.finish();
    require(!refused.classification_error && !refused.transfer_error,
            "refusal caused a failed backing access");
    for (const auto& file : refused.files)
        check(file.reads + file.writes + file.truncates + file.alternate == 0,
              "below-base refusal touched backing storage");
    std::printf("stream closed=%llu retired=%llu retired_texts=%llu\n",
        static_cast<unsigned long long>(geometry.trades),
        static_cast<unsigned long long>(retired),
        static_cast<unsigned long long>(geometry.retired_extents));
}
} // namespace

int main() {
    try {
        Directory directory;
        forwarding_control(directory);
        scenario(directory);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL closed-trade streaming I/O: %s\n", error.what());
        return EXIT_FAILURE;
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
