// Linux physical-I/O specification. Execute on the verification host.
// The reversal fixture follows the source host's market-entry fixtures;
// the wire oracle follows the independent deferred-sink-sync fixture.
#if !defined(__linux__)
#error "Physical ledger accounting requires Linux linker wrapping"
#endif
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/version.h>
#include "../src/sha256.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

namespace ledger_io {
using Count = std::uint64_t;
enum class File : std::size_t { index, payload, other };
struct Io {
    Count pwrite_calls = 0, pwrite_bytes = 0, pread_calls = 0, pread_bytes = 0;
    Count alternate_calls = 0;
};
struct Counters {
    std::array<Io, static_cast<std::size_t>(File::other)> files{};
    bool classification_error = false, transfer_error = false;
};
bool measuring = false;
Counters counters;

// Resolve every fd anew: descriptor reuse cannot inherit another file's class.
// No product counter, inferred transfer size, allocation or replacement I/O.
File identify(int fd) noexcept {
    struct stat status{};
    char path[sizeof("/proc/self/fd/") + 3 * sizeof(int)]{};
    char name[4096]{};
    const int length = std::snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(path)
        || ::fstat(fd, &status) != 0) {
        counters.classification_error = true;
        return File::other;
    }
    const auto used = ::readlink(path, name, sizeof(name) - 1);
    if (used < 0 || static_cast<std::size_t>(used) == sizeof(name) - 1) {
        counters.classification_error = true;
        return File::other;
    }
    name[static_cast<std::size_t>(used)] = '\0';
    const char* slash = std::strrchr(name, '/');
    const char* base = slash ? slash + 1 : name;
    constexpr const char* prefixes[]{"pineforge-trade-index", "pineforge-trade-payload"};
    for (std::size_t i = 0; i < std::size(prefixes); ++i) {
        const auto bytes = std::strlen(prefixes[i]);
        if (std::strncmp(base, prefixes[i], bytes) == 0 && base[bytes] == '-') {
            if (!S_ISREG(status.st_mode)) counters.classification_error = true;
            return static_cast<File>(i);
        }
    }
    return File::other;
}
void record(int fd, std::size_t bytes, ssize_t result, bool write) noexcept {
    if (!measuring) return;
    const int saved = errno;
    const auto file = identify(fd);
    if (file != File::other) {
        auto& io = counters.files[static_cast<std::size_t>(file)];
        ++(write ? io.pwrite_calls : io.pread_calls);
        if (result >= 0)
            (write ? io.pwrite_bytes : io.pread_bytes) += static_cast<Count>(result);
        counters.transfer_error |= result < 0 || static_cast<std::size_t>(result) != bytes;
    }
    errno = saved;
}
void alternate(int fd) noexcept {
    if (!measuring || fd < 0) return;
    const int saved = errno;
    const auto file = identify(fd);
    if (file != File::other)
        ++counters.files[static_cast<std::size_t>(file)].alternate_calls;
    errno = saved;
}
struct Measure {
    Measure() {
        if (measuring) throw std::runtime_error("Nested physical I/O measurement");
        counters = {};
        measuring = true;
    }
    ~Measure() { measuring = false; }
    Counters finish() {
        measuring = false;
        return counters;
    }
};
} // namespace ledger_io

extern "C" ssize_t __real_pwrite(int, const void*, std::size_t, off_t);
extern "C" ssize_t __real_pread(int, void*, std::size_t, off_t);
extern "C" ssize_t __real_write(int, const void*, std::size_t);
extern "C" ssize_t __real_read(int, void*, std::size_t);
extern "C" ssize_t __real_writev(int, const struct iovec*, int);
extern "C" ssize_t __real_readv(int, const struct iovec*, int);
extern "C" ssize_t __real_pwritev(int, const struct iovec*, int, off_t);
extern "C" ssize_t __real_preadv(int, const struct iovec*, int, off_t);
extern "C" void* __real_mmap(void*, std::size_t, int, int, int, off_t);

extern "C" ssize_t __wrap_pwrite(int fd, const void* data, std::size_t bytes, off_t offset) {
    const auto result = __real_pwrite(fd, data, bytes, offset);
    ledger_io::record(fd, bytes, result, true);
    return result;
}
extern "C" ssize_t __wrap_pread(int fd, void* data, std::size_t bytes, off_t offset) {
    const auto result = __real_pread(fd, data, bytes, offset);
    ledger_io::record(fd, bytes, result, false);
    return result;
}
extern "C" ssize_t __wrap_write(int fd, const void* data, std::size_t bytes) {
    const auto result = __real_write(fd, data, bytes);
    ledger_io::alternate(fd);
    return result;
}
extern "C" ssize_t __wrap_read(int fd, void* data, std::size_t bytes) {
    const auto result = __real_read(fd, data, bytes);
    ledger_io::alternate(fd);
    return result;
}
extern "C" ssize_t __wrap_writev(int fd, const struct iovec* data, int count) {
    const auto result = __real_writev(fd, data, count);
    ledger_io::alternate(fd);
    return result;
}
extern "C" ssize_t __wrap_readv(int fd, const struct iovec* data, int count) {
    const auto result = __real_readv(fd, data, count);
    ledger_io::alternate(fd);
    return result;
}
extern "C" ssize_t __wrap_pwritev(int fd, const struct iovec* data, int count, off_t offset) {
    const auto result = __real_pwritev(fd, data, count, offset);
    ledger_io::alternate(fd);
    return result;
}
extern "C" ssize_t __wrap_preadv(int fd, const struct iovec* data, int count, off_t offset) {
    const auto result = __real_preadv(fd, data, count, offset);
    ledger_io::alternate(fd);
    return result;
}
extern "C" void* __wrap_mmap(void* address, std::size_t length, int protection,
                              int flags, int fd, off_t offset) {
    auto* result = __real_mmap(address, length, protection, flags, fd, offset);
    if ((flags & MAP_ANONYMOUS) == 0) ledger_io::alternate(fd);
    return result;
}

namespace pineforge::detail {
// The existing ledger-fixture friend; used only by the refusal controls.
struct TradeLedgerStorageProbe {
    static void retire_history(TradeLedger& ledger) { ledger.set_history_retirement(true); }
};
} // namespace pineforge::detail

namespace {
using namespace pineforge;
using ledger_io::Count;
int failures = 0;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", message);
    }
}
void bound(const char* metric, Count actual, Count allowed, Count trades) {
    const bool ok = actual <= allowed;
    std::printf("%s total=%llu per_closed_trade=%.9Lf allowed=%llu T=%llu %s\n",
        metric, static_cast<unsigned long long>(actual),
        static_cast<long double>(actual) / trades, static_cast<unsigned long long>(allowed),
        static_cast<unsigned long long>(trades), ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}
void valid(const ledger_io::Counters& io) {
    require(!io.classification_error && !io.transfer_error,
            "Physical ledger I/O classification failed, or transfer was short/failed");
    for (const auto& file : io.files)
        require(file.alternate_calls == 0, "Ledger used unaccounted alternate physical I/O");
}
struct Directory {
    std::string path;
    Directory() {
        std::string refusal;
        int error = 0;
        // Use the production disk-directory selection, including its tmpfs
        // refusal/fallback, before selecting a unique child for this fixture.
        path = detail::resolve_scratch_directory("", refusal, error);
        require(!path.empty(), "Cannot select disk-backed fixture directory: " + refusal);
        path += "/pineforge-closed-trade-io-XXXXXX";
        require(::mkdtemp(path.data()) != nullptr, "Cannot create fixture directory");
    }
    ~Directory() {
        ::unlink((path + "/report.ndjson").c_str());
        ::rmdir(path.c_str());
    }
};

// Thirty-two descriptor windows yield 2,048 closed rows, well below the
// default decoded-cache cap. Signal geometry, not an observed I/O count.
constexpr std::size_t kClosedRows = 32 * TradeLedger::kResidentRows;
constexpr std::int64_t kStart = INT64_C(1704067200000), kStep = 60000;
constexpr double kCapital = 1000000.0, kPrice = 100.0, kQuantity = 1.0;
constexpr char kRunId[] = "closed-trade-ledger";
static_assert(kClosedRows >= 2000 && kClosedRows < TradeLedger::kDefaultCacheRows);
const char* entry_id(std::size_t signal) { return signal % 2 == 0 ? "Long" : "Short"; }

std::vector<Bar> tape() {
    std::vector<Bar> bars;
    // Entries at [0,T), flatten at T, fill at T+1, then one quiet bar.
    for (std::size_t i = 0; i < kClosedRows + 3; ++i)
        bars.push_back({kPrice, kPrice, kPrice, kPrice, 1.0,
                       kStart + static_cast<std::int64_t>(i) * kStep});
    return bars;
}
struct Host final : source::PineStrategyHost {
    std::size_t callbacks = 0;
    Host() {
        source::PineStrategyConfig config;
        config.initial_capital = kCapital;
        config.default_qty_value = kQuantity;
        configure_pine_strategy(config);
        // Retain indexed history: no physical retirement and no interleaved
        // seal T output. This is the ordinary history-reading report route.
        set_trade_history_streamable(false);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        syminfo_mintick_ = 0.25;
    }
    void on_source_bar(const Bar& bar) override {
        const auto signal = callbacks++;
        require(pine_bar_index() == static_cast<int>(signal)
                && bar.timestamp == kStart + static_cast<std::int64_t>(signal) * kStep,
                "Source callback skipped, duplicated or reordered an input");
        if (signal < kClosedRows) {
            const auto missing = std::numeric_limits<double>::quiet_NaN();
            strategy_entry(entry_id(signal), signal % 2 == 0, missing, missing, kQuantity, "");
        } else if (signal == kClosedRows) {
            strategy_close_all();
        }
    }
    std::size_t cache_rows() const { return trades_.cache_rows(); }
};

// Independent v1 header, as in the deferred-sink-sync known-answer fixture.
// No production formatter, report accessor or measured stream builds it.
std::string header_line() {
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
std::string integer_real(double value) {
    const auto integer = static_cast<std::int64_t>(value);
    require(value == static_cast<double>(integer), "Oracle requires exact integral prices/equity");
    return std::to_string(integer) + ".0";
}
struct Golden {
    std::vector<Trade> rows;
    std::string prefix, digest, stream;
};
Golden recompute(const std::vector<Bar>& bars) {
    Golden result;
    result.prefix = header_line();
    // Equal OHLC, no commissions/slippage and unit reversals imply zero
    // realized and open P&L, zero excursions, unchanged initial equity.
    for (const auto& bar : bars)
        result.prefix += "[\"E\"," + std::to_string(bar.timestamp) + ","
            + integer_real(kCapital) + "," + integer_real((bar.close - bar.open) * kQuantity)
            + "]\n";
    for (std::size_t signal = 0; signal < kClosedRows; ++signal) {
        Trade row{};
        row.entry_bar_index = static_cast<int>(signal + 1);
        row.exit_bar_index = row.entry_bar_index + 1;
        row.entry_time = bars[static_cast<std::size_t>(row.entry_bar_index)].timestamp;
        row.exit_time = bars[static_cast<std::size_t>(row.exit_bar_index)].timestamp;
        row.entry_price = bars[static_cast<std::size_t>(row.entry_bar_index)].open;
        row.exit_price = bars[static_cast<std::size_t>(row.exit_bar_index)].open;
        row.qty = kQuantity;
        row.is_long = signal % 2 == 0;
        row.entry_id = entry_id(signal);
        row.exit_id = signal + 1 < kClosedRows ? entry_id(signal + 1) : "__close__";
        row.entry_incarnation = signal + 1; // One accepted request per source calculation.
        row.pnl = (row.is_long ? row.exit_price - row.entry_price
                               : row.entry_price - row.exit_price) * row.qty;
        row.pnl_pct = row.pnl / (row.entry_price * row.qty) * 100.0;
        result.rows.push_back(row);
        result.prefix += "[\"T\"," + std::to_string(signal + 1)
            + (row.is_long ? ",\"long\"," : ",\"short\",")
            + std::to_string(row.entry_time) + "," + std::to_string(row.exit_time);
        for (double value : {row.entry_price, row.exit_price, row.qty, row.pnl, row.pnl_pct,
                             row.max_runup, row.max_drawdown, row.commission})
            result.prefix += "," + integer_real(value);
        result.prefix += "," + std::to_string(row.entry_bar_index) + ","
            + std::to_string(row.exit_bar_index) + ",false,"
            + std::to_string(row.entry_incarnation) + ","
            + std::to_string(static_cast<int>(execution::CloseCause::Script)) + ",\""
            + row.entry_id + "\",\"" + row.exit_id + "\",\"\"]\n";
    }
    report_sink::Sha256 hash;
    hash.update(result.prefix);
    result.digest = hash.hex_digest();
    result.stream = result.prefix
        + "{\"trailer\":\"pf-report-sink\",\"version\":1,\"counts\":{\"E\":"
        + std::to_string(bars.size()) + ",\"T\":" + std::to_string(result.rows.size())
        + "},\"sha256\":\"" + result.digest + "\"}\n";
    return result;
}
std::string contents(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "Cannot read completed report");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
struct Report {
    ReportC value{};
    ~Report() { BacktestEngine::free_report(&value); }
};

void forwarding_control(const Directory& directory) {
    report_sink::Sha256 known_answer;
    known_answer.update("abc");
    require(known_answer.hex_digest()
                == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "SHA-256 FIPS known-answer control failed");
    detail::ScratchFile index, payload;
    index.set_directory(directory.path);
    payload.set_directory(directory.path);
    index.open("pineforge-trade-index");
    payload.open("pineforge-trade-payload");
    const std::array<unsigned char, TradeLedger::kRecordBytes> descriptor{};
    const std::string text = entry_id(0);
    auto copy = descriptor;
    std::string copied(text.size(), '\0');
    ledger_io::Measure measurement;
    index.write(0, descriptor.data(), descriptor.size());
    payload.write(0, text.data(), text.size());
    index.read(0, copy.data(), copy.size());
    payload.read(0, copied.data(), copied.size());
    const auto io = measurement.finish();
    valid(io);
    const auto& a = io.files[static_cast<std::size_t>(ledger_io::File::index)];
    const auto& b = io.files[static_cast<std::size_t>(ledger_io::File::payload)];
    require(a.pwrite_calls == 1 && a.pread_calls == 1
            && a.pwrite_bytes == descriptor.size() && a.pread_bytes == descriptor.size()
            && b.pwrite_calls == 1 && b.pread_calls == 1
            && b.pwrite_bytes == text.size() && b.pread_bytes == text.size()
            && copy == descriptor && copied == text,
            "Real syscall forwarding/classification control failed");
}
template<class Operation> void invariant(Operation operation) {
    bool refused = false;
    try { operation(); }
    catch (const std::logic_error& error) {
        require(dynamic_cast<const RunFailureInfo*>(&error) != nullptr
                && classify_run_failure(error).code == RunFailureCode::engine_invariant,
                "Ledger refusal lost its registered typed failure");
        refused = true;
    }
    check(refused, "Ledger accepted a sealed edit or retired-history read");
}
void refusal_controls(const Directory& directory) {
    Trade row{};
    row.entry_id = entry_id(0);
    TradeLedger retained;
    retained.set_scratch_directory(directory.path);
    retained.push_back(row);
    retained.seal(retained.size());
    invariant([&] { retained.replace(0, row); }); // Even an unchanged row must be refused.
    invariant([&] { retained.modify(0, [](Trade&) {}); });
    check(retained.at(0).entry_id == row.entry_id, "Retaining seal lost readable history");
    TradeLedger retired;
    retired.set_scratch_directory(directory.path);
    detail::TradeLedgerStorageProbe::retire_history(retired);
    for (std::size_t i = 0; i < 2 * TradeLedger::kResidentRows; ++i) retired.push_back(row);
    retired.seal(retired.size());
    invariant([&] { (void)retired.at(0); });
    invariant([&] { (void)retired.numeric_at(0); });
    invariant([&] { (void)retired.entry_id_at(0); });
}

void scenario(const Directory& directory) {
    const auto bars = tape();
    const auto golden = recompute(bars);
    Host host;
    const std::string path = directory.path + "/report.ndjson";
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), kRunId, directory.path.c_str()};
    require(host.set_report_sink_v1(&sink) == 0, "Report sink setup refused");
    ledger_io::Measure measurement;
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false, 4,
             MagnifierDistribution::ENDPOINTS);
    const auto io = measurement.finish();
    require(host.last_run_status() == 0 && host.last_error().empty(),
            "Source run failed: " + host.last_error());
    require(host.callbacks == bars.size() && host.closed_trade_count() == golden.rows.size()
            && host.physical_position().signed_units == 0.0 && host.pending_order_count() == 0,
            "Reversal driver did not produce the complete flat closed-trade population");
    require(host.closed_trade_count() < host.cache_rows()
            && host.cache_rows() == TradeLedger::kDefaultCacheRows,
            "Row population exceeds the declared resident-cache geometry");
    valid(io);

    Count extents = 0, text_bytes = 0, cacheable_text_rows = 0;
    for (std::size_t i = 0; i < golden.rows.size(); ++i) {
        const auto row = host.closed_trade(i);
        const auto& expected = golden.rows[i];
        check(row.entry_id == expected.entry_id && row.exit_id == expected.exit_id
              && row.entry_comment.empty() && row.exit_comment.empty(),
              "Produced row text differs from independent reversal geometry");
        Count bytes = 0;
        for (const auto* text : {&row.entry_id, &row.entry_comment, &row.exit_comment, &row.exit_id}) {
            extents += !text->empty();
            bytes += text->size();
        }
        require(bytes <= TradeLedger::kCacheStringBytes, "Row text exceeds packed resident cache");
        cacheable_text_rows += bytes <= TradeLedger::kCacheStringBytes;
        text_bytes += bytes;
    }
    const auto trades = static_cast<Count>(host.closed_trade_count());
    const auto& index = io.files[static_cast<std::size_t>(ledger_io::File::index)];
    const auto& payload = io.files[static_cast<std::size_t>(ledger_io::File::payload)];
    // (a) calls/T <= T/T: one descriptor publication per final row.
    // src/engine_orders.cpp:637-659,1010-1020; redundant host replace:
    // src/source/pine_strategy_host.cpp:1120-1132. Byte budget = T*kRecordBytes
    // (include/pineforge/trade_ledger.hpp:40); syscall splits count in full.
    bound("descriptor-write count", index.pwrite_calls, trades, trades);
    bound("index.pwrite_bytes", index.pwrite_bytes, trades * TradeLedger::kRecordBytes, trades);
    // (b) calls/T <= sum_rows sum_four_fields(!empty)/T; bytes <= sum(length).
    // put_string writes only nonempty extents: src/engine_orders.cpp:572-579;
    // store/reuse preserve equal extents: :633-655. Derive from PRODUCED rows.
    bound("payload.pwrite_calls", payload.pwrite_calls, extents, trades);
    bound("payload.pwrite_bytes", payload.pwrite_bytes, text_bytes, trades);
    // The complete fixed-width index must reach disk; payload deduplication
    // may reduce its byte traffic, but cannot remove the backing altogether.
    require(index.pwrite_bytes >= trades * TradeLedger::kRecordBytes
            && (text_bytes == 0 || payload.pwrite_bytes != 0),
            "Physical scratch writes disappeared from accounting");
    // (c) reads/T <= (T-min(T,cache_rows,cacheable_text_rows))/T = 0:
    // short current rows are resident
    // during execution; src/engine_orders.cpp:412-424,490-493,
    // :582-599,770-775. T < kDefaultCacheRows, sum(text lengths) <= 128.
    // Deliberate cold report/history materialization is OUTSIDE run accounting.
    const Count cold_required = trades - std::min(
        {trades, static_cast<Count>(host.cache_rows()), cacheable_text_rows});
    bound("index.pread_calls", index.pread_calls, cold_required, trades);
    bound("payload.pread_calls", payload.pread_calls, cold_required, trades);
    std::printf("index.pread_bytes=%llu payload.pread_bytes=%llu\n",
        static_cast<unsigned long long>(index.pread_bytes),
        static_cast<unsigned long long>(payload.pread_bytes));

    // The first pass above populated all decoded rows after any growth
    // invalidations. A second full-history read must require no disk reads.
    ledger_io::Measure warm_measurement;
    for (std::size_t i = 0; i < golden.rows.size(); ++i) {
        const auto row = host.closed_trade(i);
        check(row.entry_id == golden.rows[i].entry_id && row.pnl == golden.rows[i].pnl,
              "Warm history differs from the independent row");
    }
    const auto warm = warm_measurement.finish();
    valid(warm);
    bound("warm.index.pread_calls", warm.files[0].pread_calls, cold_required, trades);
    bound("warm.payload.pread_calls", warm.files[1].pread_calls, cold_required, trades);

    // (d) Entire wire stream equality, not a projection or a self-hash:
    // E from every OHLC bar, T from next-open unit reversals, v1 header and
    // trailer; src/report_sink_writer.cpp:563-613,628-665. No product
    // serializer or product rows build the expected E/T lines or digest.
    Report report;
    host.fill_report(&report.value);
    pf_report_sink_receipt_v1 receipt{sizeof receipt, 1};
    require(host.report_sink_receipt_v1(&receipt) == 0 && receipt.complete == 1,
            "Completed source report has no complete sink receipt");
    const auto actual = contents(path);
    check(actual == golden.stream, "Complete report E/T/header/trailer bytes differ from golden");
    if (actual != golden.stream) {
        const auto end = std::mismatch(actual.begin(), actual.end(),
                                       golden.stream.begin(), golden.stream.end());
        std::fprintf(stderr, "report first_difference=%llu actual_bytes=%llu golden_bytes=%llu\n",
            static_cast<unsigned long long>(std::distance(actual.begin(), end.first)),
            static_cast<unsigned long long>(actual.size()),
            static_cast<unsigned long long>(golden.stream.size()));
    }
    check(receipt.count_e == static_cast<std::int64_t>(bars.size())
          && receipt.count_t == static_cast<std::int64_t>(golden.rows.size())
          && golden.digest == receipt.sha256_hex, "Report counts or stream SHA-256 differ from golden");
    std::printf("report E=%lld T=%lld golden_sha256=%s actual_sha256=%s\n",
        static_cast<long long>(receipt.count_e), static_cast<long long>(receipt.count_t),
        golden.digest.c_str(), receipt.sha256_hex);
}
} // namespace

int main() {
    try {
        Directory directory;
        forwarding_control(directory);
        scenario(directory);
        refusal_controls(directory);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL closed-trade scratch I/O: %s\n", error.what());
        return EXIT_FAILURE;
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
