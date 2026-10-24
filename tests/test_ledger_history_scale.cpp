// Compile this same fixture on the unchanged tree to produce the independent
// --write-reference artifacts; --compare-reference streams them one row at a
// time. The default also checks an append-time oracle before any eviction.
#include "../src/native_execution_consumer.hpp"
#include <pineforge/report_sink.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace {
using namespace pineforge;
constexpr std::int64_t start_ms = INT64_C(1704067200000);
constexpr double missing = std::numeric_limits<double>::quiet_NaN();

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T>
void append(std::string& bytes, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
}
void append(std::string& bytes, const std::string& value) {
    append(bytes, static_cast<std::uint64_t>(value.size()));
    bytes.append(value);
}
std::string encoded(const Trade& row) {
    std::string bytes;
    append(bytes, row.entry_time); append(bytes, row.exit_time);
    append(bytes, row.entry_price); append(bytes, row.exit_price);
    append(bytes, row.qty); append(bytes, row.pnl); append(bytes, row.pnl_pct);
    append(bytes, row.is_long);
    append(bytes, row.entry_bar_index); append(bytes, row.exit_bar_index);
    append(bytes, row.entry_id); append(bytes, row.entry_comment);
    append(bytes, row.exit_comment); append(bytes, row.exit_id);
    append(bytes, row.exit_from_bracket);
    append(bytes, row.max_runup); append(bytes, row.max_drawdown); append(bytes, row.commission);
    append(bytes, row.entry_incarnation); append(bytes, row.open_at_end);
    append(bytes, row.close_cause);
    return bytes;
}
// Test artifacts also use fixed buffers and release their I/O cache. Otherwise
// the comparison itself could charge O(T) dirty pages to a memory probe.
class Records {
public:
    Records(const std::string& path, bool writing) : writing_(writing) {
        file_ = std::fopen(path.c_str(), writing ? "wb" : "rb");
        require(file_ != nullptr, "cannot open reference fixture");
#if defined(__linux__)
        require(::posix_fadvise(::fileno(file_), 0, 0, POSIX_FADV_RANDOM) == 0,
                "cannot configure reference read window");
#elif defined(__APPLE__)
        require(::fcntl(::fileno(file_), F_NOCACHE, 1) == 0,
                "cannot configure reference cache");
#endif
    }
    ~Records() { if (file_) std::fclose(file_); }
    Records(const Records&) = delete;
    Records& operator=(const Records&) = delete;
    void write(const std::string& bytes) {
        const auto length = static_cast<std::uint64_t>(bytes.size());
        require(std::fwrite(&length, sizeof(length), 1, file_) == 1
                && std::fwrite(bytes.data(), 1, bytes.size(), file_) == bytes.size(),
                "cannot write reference record");
        pending_ += sizeof(length) + bytes.size();
        if (pending_ >= 64 * 1024) flush();
    }
    std::string read() {
        std::uint64_t length = 0;
        require(std::fread(&length, sizeof(length), 1, file_) == 1,
                "missing reference record length");
        // All fixture strings have fixed widths. Reject a corrupt artifact
        // before allocation; this is not a production capacity policy.
        require(length < 4096, "invalid reference fixture record");
        std::string bytes(static_cast<std::size_t>(length), '\0');
        require(std::fread(bytes.data(), 1, bytes.size(), file_) == bytes.size(),
                "missing reference record");
        discard_cache();
        return bytes;
    }
    bool eof() { return std::fgetc(file_) == EOF && !std::ferror(file_); }
    void close() {
        if (writing_) flush();
        auto* closed = std::exchange(file_, nullptr);
        require(std::fclose(closed) == 0, "cannot close reference fixture");
    }
private:
    void discard_cache() {
#if defined(__linux__)
        require(::posix_fadvise(::fileno(file_), 0, 0, POSIX_FADV_DONTNEED) == 0,
                "cannot discard reference cache");
#endif
    }
    void flush() {
        require(std::fflush(file_) == 0, "cannot flush reference fixture");
#if defined(__linux__)
        int result;
        do { result = ::fdatasync(::fileno(file_)); } while (result != 0 && errno == EINTR);
        require(result == 0, "cannot flush reference disk window");
#endif
        discard_cache();
        pending_ = 0;
    }
    std::FILE* file_ = nullptr;
    bool writing_ = false;
    std::size_t pending_ = 0;
};
void fold_row(BrokerStateHashSink& fold, const Trade& row) {
    // Unchanged-tree vector digest: exact canonical six-field left fold.
    fold.i(row.entry_time); fold.i(row.exit_time);
    fold.d(row.entry_price); fold.d(row.exit_price);
    fold.d(row.qty); fold.d(row.pnl);
}

// Keep the population/bound test compilable on the unchanged header closure.
template<class L>
auto resident(const L& ledger, int) -> decltype(ledger.resident_size()) {
    return ledger.resident_size();
}
template<class L>
std::size_t resident(const L& ledger, long) { return ledger.size() - ledger.base(); }
template<class C>
auto checkpoints(const C& consumer, int) -> decltype(consumer.resident_closed_row_checkpoints()) {
    return consumer.resident_closed_row_checkpoints();
}
template<class C>
std::size_t checkpoints(const C& consumer, long) { return consumer.closed_rows_final(); }
template<class A, class = void> struct HasPhaseValues : std::false_type {};
template<class A> struct HasPhaseValues<A, std::void_t<
    decltype(std::declval<const A&>().exit_phase_prefix()),
    decltype(std::declval<const A&>().exit_phase_value(0)),
    decltype(std::declval<const A&>().resident_exit_phases())>> : std::true_type {};

struct Directory {
    std::string path;
    Directory() {
        const char* root = std::getenv("TMPDIR");
        path = root && *root ? root : "/tmp";
        path += "/pineforge-ledger-history-XXXXXX";
        require(::mkdtemp(path.data()) != nullptr, "cannot create fixture directory");
    }
    ~Directory() {
        ::unlink((std::string(path) + "/append.bin").c_str());
        ::unlink((std::string(path) + "/report.ndjson").c_str());
        ::rmdir(path.c_str());
    }
};

class Population final : public source::PineStrategyHost {
public:
    Population(std::size_t count, Records& expected) : count_(count), expected_(expected) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000000.0;
        config.margin_long = config.margin_short = 0.0;
        config.commission_value = 0.00125;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        syminfo_mintick_ = 0.01;
    }
    const TradeLedger& ledger() const { return trades_; }
    std::size_t observed = 0, peak_rows = 0, peak_checkpoints = 0;
    BrokerStateHashSink expected_rows;
    std::uint64_t expected_phases = UINT64_C(1469598103934665603);

    void on_source_bar(const Bar&) override {
        sample();
        if (bar_index_ == 0)
            strategy_entry("seed", true, missing, missing, static_cast<double>(count_ + 1));
        if (bar_index_ == 1) {
            // One physical opening, one script bar, an arbitrarily long
            // amendable exit-time block. The conservative seal cannot retire
            // any of it, even though a report sink is installed.
            for (std::size_t i = 0; i < count_; ++i)
                strategy_close("seed", "closed history", 1.0, missing, true);
        }
    }
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext& context) override {
        for (std::size_t i = 0; i < event.closed_trade_count; ++i) {
            const Trade row = get_trade(static_cast<int>(event.first_trade_index + i));
            std::string bytes = encoded(row);
            const auto phase = static_cast<std::uint8_t>(context.coordinate.path_phase);
            if (observed == 0) phase_seen_ = phase;
            require(phase == phase_seen_, "same-bar fixture unexpectedly mixes exit phases");
            append(bytes, phase);
            expected_.write(bytes);
            fold_row(expected_rows, row);
            expected_phases = source::PineExecutionAdapter::fold_exit_phase(expected_phases, phase);
            ++observed;
        }
        sample();
    }
    void sample() {
        peak_rows = std::max(peak_rows, resident(trades_, 0));
        const auto& consumer = NativeExecutionConsumer::bound(*this);
        peak_checkpoints = std::max(peak_checkpoints, checkpoints(consumer, 0));
    }
    std::uint64_t adapter_hash() const {
        BrokerStateHashSink fold;
        adapter_.hash_state(fold);
        return fold.h;
    }
    template<class A>
    void phase_checks(const A& adapter) const {
        if constexpr (HasPhaseValues<A>::value) {
            const auto prefix = adapter.exit_phase_prefix();
            require(prefix.first == count_, "phase fold did not consume the real population");
            require(prefix.second == expected_phases, "phase fold differs from append-time oracle");
            require(adapter.resident_exit_phases() <= 64 * 1024, "resident phase history grew");
        }
    }
    void check_phases() const { phase_checks(adapter_); }
    template<class A>
    std::uint8_t phase_at(const A& adapter, std::size_t index) const {
        if constexpr (HasPhaseValues<A>::value) return adapter.exit_phase_value(index);
        else {
            // The baseline artifact already captured the one production
            // writer's phase from its applied callback above.
            (void)adapter; (void)index;
            return phase_seen_;
        }
    }
    std::string observed_record(std::size_t index) const {
        std::string bytes = encoded(closed_trade(index));
        append(bytes, phase_at(adapter_, index));
        return bytes;
    }
    void amend_first_row() {
        Trade row = get_trade(0);
        row.pnl += 0.125;
        // Both branches are compiled only for their header's API.
        replace_row(trades_, row, 0);
        native_closed_rows_amended(0);
    }
    template<class L>
    static auto replace_row(L& ledger, const Trade& row, int)
            -> decltype(ledger.replace(0, row), void()) {
        ledger.replace(0, row);
    }
    template<class L>
    static void replace_row(L& ledger, const Trade& row, long) { ledger[0] = row; }
private:
    std::size_t count_;
    Records& expected_;
    std::uint8_t phase_seen_ = 0;
};

std::string summary(Population& host) {
    const auto& consumer = NativeExecutionConsumer::bound(host);
    std::string bytes;
    append(bytes, static_cast<std::uint64_t>(host.closed_trade_count()));
    append(bytes, consumer.closed_rows_digest(host.ledger()));
    append(bytes, host.expected_phases);
    append(bytes, host.adapter_hash());
    append(bytes, host.broker_state_hash());
    return bytes;
}

void run(std::size_t count, const std::string& artifact_prefix, bool write_reference) {
    Directory directory;
    const std::string scratch = directory.path;
    const std::string oracle_path = scratch + "/append.bin";
    const std::string sink_path = scratch + "/report.ndjson";
    const std::string artifact = artifact_prefix.empty() ? std::string{}
        : artifact_prefix + "-" + std::to_string(count) + ".bin";
    Records oracle(oracle_path, true);
    Population host(count, oracle);
    pf_report_sink_v1 sink{};
    sink.version = 1;
    sink.struct_size = sizeof(sink);
    sink.path = sink_path.c_str();
    sink.scratch_dir = scratch.c_str();
    require(host.set_report_sink_v1(&sink) == 0, "cannot install report sink");
    host.set_trade_history_streamable(true);
    const std::array<Bar, 4> bars{{
        {100, 101, 99, 100, 1, start_ms},
        {101, 102, 100, 101, 1, start_ms + 60000},
        {102, 103, 101, 102, 1, start_ms + 120000},
        {103, 104, 102, 103, 1, start_ms + 180000}
    }};
    host.run(bars.data(), static_cast<int>(bars.size()));
    require(host.last_error().empty(), host.last_error().c_str());
    require(host.observed == count, "fixture did not close the requested real population");
    require(host.closed_trade_count() == count, "real closed-trade count is wrong");
    require(host.ledger().base() == 0, "fixture did not pin its full exit-time block");
    require(host.get_trade(0).exit_time == host.get_trade(static_cast<int>(count - 1)).exit_time,
            "fixture trades do not share one exit time");
    oracle.close();

    Records expected(oracle_path, false);
    std::unique_ptr<Records> unchanged, reference;
    if (!artifact.empty()) {
        if (write_reference) reference = std::make_unique<Records>(artifact, true);
        else unchanged = std::make_unique<Records>(artifact, false);
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto live = host.observed_record(i);
        auto before_eviction = expected.read();
        // On the unchanged tree the phase read API does not exist. Preserve
        // its actual callback byte rather than manufacturing one.
        if (write_reference) reference->write(before_eviction);
        else require(live == before_eviction, "historical row/phase differs after eviction");
        if (unchanged)
            require(live == unchanged->read(), "row/phase differs from unchanged-tree run");
    }
    require(expected.eof(), "extra append-time rows");
    const auto state = summary(host);
    if (write_reference) reference->write(state);
    if (unchanged) {
        require(state == unchanged->read(), "digest, phase or hash leaf differs from unchanged tree");
    }
    require(NativeExecutionConsumer::bound(host).closed_rows_digest(host.ledger())
                == host.expected_rows.h, "closed-row digest differs from original vector fold");
    if (!write_reference) {
        host.sample();
        require(host.peak_rows <= 64, "resident Trade rows grew with the real population");
        require(host.peak_checkpoints <= 64, "resident digest checkpoints grew with trades");
        host.check_phases();
    }
    // Final report streaming must reread the entire externally stored block.
    ReportC report{};
    host.fill_report(&report);
    require(report.total_trades == static_cast<int>(count + 1),
            "report lost closed rows or its one remaining open lot");
    require(report.trades_len == 0 && report.trades == nullptr, "sink materialized closed trades");
    std::string metrics;
    append(metrics, report.total_trades);
    append(metrics, report.net_profit);
    append(metrics, report.metrics);
    if (write_reference) reference->write(metrics);
    if (unchanged) {
        require(metrics == unchanged->read(), "report or metric bits differ from unchanged tree");
        require(unchanged->eof(), "extra unchanged-tree rows");
    }
    const int report_count = report.total_trades;
    BacktestEngine::free_report(&report);
    pf_report_sink_receipt_v1 receipt{};
    receipt.version = 1;
    receipt.struct_size = sizeof(receipt);
    require(host.report_sink_receipt_v1(&receipt) == 0, "cannot read report sink receipt");
    require(receipt.complete && receipt.count_t == report_count,
            "report sink did not consume all real rows");
    if (reference) reference->close();
    if (!write_reference) {
        // Rewind before every retained checkpoint, while row zero exists only
        // in scratch. Readback above has already compared every unchanged row
        // with the independent append-time/unchanged-tree source.
        host.amend_first_row();
        BrokerStateHashSink amended;
        for (std::size_t i = 0; i < count; ++i) fold_row(amended, host.closed_trade(i));
        require(amended.h != host.expected_rows.h, "amendment did not change the reference digest");
        require(NativeExecutionConsumer::bound(host).closed_rows_digest(host.ledger()) == amended.h,
                "amendment below the checkpoint window did not replay the prefix");
        require(NativeExecutionConsumer::bound(host).closed_rows_digest_holds(host.ledger()),
                "replayed amended digest does not match exact external rows");
    }
    require(::unlink(oracle_path.c_str()) == 0, "cannot remove append-time oracle");
    require(::unlink(sink_path.c_str()) == 0, "cannot remove report sink");
    std::printf("ledger history: %zu real trades, peak %zu rows, %zu checkpoints\n",
                count, host.peak_rows, host.peak_checkpoints);
}
} // namespace

int main(int argc, char** argv) {
    try {
        const bool write_reference = argc == 3 && std::strcmp(argv[1], "--write-reference") == 0;
        const bool compare = argc == 3 && std::strcmp(argv[1], "--compare-reference") == 0;
        require(argc == 1 || write_reference || compare, "invalid history fixture arguments");
        const std::string prefix = argc == 3 ? argv[2] : "";
#ifdef PINEFORGE_LEDGER_HISTORY_400K
        run(400000, prefix, write_reference);
#elif defined(PINEFORGE_LEDGER_HISTORY_4K)
        run(4000, prefix, write_reference);
#else
        run(100000, prefix, write_reference);
#endif
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ledger history failure: %s\n", error.what());
        return 1;
    }
}
