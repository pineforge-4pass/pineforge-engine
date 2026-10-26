// CR-364: reuse the existing real disk accounting, independent trading driver,
// memory reference and semantic report encoder. The renamed entry is never run.
// All successful syscalls still forward to libc. These delegates inject only
// at an armed production I/O boundary, and obtain EBADF from the real kernel.
#define main perf_nopartial_unused_cr257_entry
#define __real_pread perf_nopartial_pread_delegate
#define __real_pwrite perf_nopartial_pwrite_delegate
#define __real_fdatasync perf_nopartial_fdatasync_delegate
#include "test_cr257_source_exit_scratch_io.cpp"
#undef __real_fdatasync
#undef __real_pwrite
#undef __real_pread
#undef main

#include "perf_nopartial_rows.hpp"
#include "report_sink_failure_fixture.hpp"
#include <pineforge/native_module.hpp>
#include <pineforge/report_mode.h>

extern "C" ssize_t __real_pread(int, void*, std::size_t, off_t);
extern "C" ssize_t __real_pwrite(int, const void*, std::size_t, off_t);
extern "C" int __real_fdatasync(int);

namespace perf_nopartial {
namespace pd = pineforge::detail;
namespace sd = pineforge::source::detail;
using pineforge::BacktestEngine;
using pineforge::ReportC;
using pineforge::RunFailureCode;
using cr257::Count;
using cr257::require;

namespace {
enum class Operation { none, read, write, sync };
struct Injection {
    Operation operation = Operation::none;
    bool capture_command = false;
    bool sink = false;
    bool armed = false;
    bool in_scope_release = false;
    bool failed_in_scope_release = false;
    int command_fd = -1;
    Count failed = 0;
    int observed_errno = 0;
};
Injection injection;

bool command_fd(int fd) {
    const auto* file = cr257::identify_fd(fd);
    return file && file->file_class == cr257::FileClass::command;
}
bool sink_fd(int fd) {
    const auto* file = cr257::identify_fd(fd);
    if (!file) return false;
    const auto* slash = std::strrchr(file->name.data(), '/');
    return std::strcmp(slash ? slash + 1 : file->name.data(), "report.jsonl") == 0;
}
bool inject(int fd, Operation operation) {
    if (!injection.armed || operation != injection.operation) return false;
    if (injection.sink ? !sink_fd(fd) : fd != injection.command_fd) return false;
    injection.armed = false; // Exactly one syscall failure, never a retry storm.
    injection.failed_in_scope_release = injection.in_scope_release;
    return true;
}
void observed_failure(long result) {
    if (result < 0) {
        ++injection.failed;
        injection.observed_errno = errno;
    }
}
struct Disarm {
    ~Disarm() { injection.armed = injection.capture_command = false; }
};

// Lab artifacts stay in the CTest working directory for review. The edit lane
// never creates them or runs this code, and this fixture deletes no artifact.
struct Artifacts {
    std::string directory = "./perf-nopartial-XXXXXX";
    std::string path;
    Artifacts() {
        require(::mkdtemp(directory.data()) != nullptr, "SETUP: create lab artifact directory");
        path = directory + "/report.jsonl";
    }
};

cr257::Canonical scalars(const ReportC& r) {
    cr257::Canonical out;
#define NOPARTIAL_FIELD(field) out.add(r.field)
    NOPARTIAL_FIELD(total_trades); NOPARTIAL_FIELD(trades_len);
    NOPARTIAL_FIELD(net_profit); NOPARTIAL_FIELD(input_bars_processed);
    NOPARTIAL_FIELD(script_bars_processed); NOPARTIAL_FIELD(security_feeds_total);
    NOPARTIAL_FIELD(security_eval_complete_total); NOPARTIAL_FIELD(security_eval_partial_total);
    NOPARTIAL_FIELD(magnifier_sub_bars_total); NOPARTIAL_FIELD(magnifier_sample_ticks_total);
    NOPARTIAL_FIELD(input_tf_seconds); NOPARTIAL_FIELD(script_tf_seconds);
    NOPARTIAL_FIELD(script_tf_ratio); NOPARTIAL_FIELD(needs_aggregation);
    NOPARTIAL_FIELD(bar_magnifier_enabled); NOPARTIAL_FIELD(security_diag_len);
    NOPARTIAL_FIELD(trace_len); NOPARTIAL_FIELD(trace_names_len);
    NOPARTIAL_FIELD(equity_curve_len); NOPARTIAL_FIELD(broker_state_hash_len);
#undef NOPARTIAL_FIELD
    out.stats(r.metrics.all); out.stats(r.metrics.longs); out.stats(r.metrics.shorts);
    out.equity_stats(r.metrics.equity);
    return out;
}
bool empty(const ReportC& r) {
    return !r.trades && !r.security_diag && !r.trace && !r.trace_names
        && !r.equity_curve && !r.broker_state_hash
        && scalars(r).bytes == scalars(ReportC{}).bytes;
}
struct Owned {
    ReportC value{};
    ~Owned() { BacktestEngine::free_report(&value); }
};

class FaultHost final : public pineforge::source::PineStrategyHost {
public:
    const cr257::Driver& driver;
    Fault fault;
    Count callbacks = 0, rows_before_fault = 0, raises = 0;
    bool snapshot_observed = false;
    bool caught_origin = false;
    RunFailureCode origin = RunFailureCode::none;
    std::string origin_args;
    explicit FaultHost(const cr257::Driver& input, Fault selected)
        : driver(input), fault(selected) {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(pineforge::QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.process_orders_on_close = false;
        configure_pine_strategy(config);
        set_trade_history_streamable(false);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        syminfo_mintick_ = 0.25;
        set_trace_enabled(true);
        set_broker_state_hash_recording(true);
        declare_outputs(1, 1, 1);
        require(set_outputs_enabled(true), "SETUP: enable real recorded outputs");
        if (fault == Fault::capacity) {
            // The repeated driver uses one entry ID. Admit exactly that
            // cohort, then request a second ID only in the final callback.
            // Other regions are deliberately generous, with no sampled bound.
            pf_scratch_tables_v1 tables{};
            tables.struct_size = sizeof tables;
            tables.version = 1;
            tables.backend = PF_SCRATCH_TABLE_BOUNDED_MEMORY;
            tables.K_e = tables.K_c = 512;
            tables.B_e = tables.B_c = 65536;
            tables.U = 1;
            tables.B_U = std::strlen(cr257::kEntryId);
            tables.V = 27;
            tables.B_V = 65536;
            tables.R = 1024;
            require(strategy_set_scratch_tables_v1(static_cast<BacktestEngine*>(this), &tables) == 0,
                    "SETUP: install bounded cohort scratch before run");
        }
    }
    void prepare_script_run(const pineforge::Bar*, int, bool) override {
        trades_.set_scratch_directory(".");
    }
    void arm(Operation operation, bool sink = false) {
        injection.operation = operation;
        injection.sink = sink;
        injection.armed = true;
    }
    void perform_fault() {
        if (fault == Fault::typed) {
            // Real recorder contract fault, after the valid output prefix.
            output_value(1, 23.0); // Only slot 0 was declared.
        } else if (fault == Fault::capacity) {
            // The production cohort table refuses its second distinct ID.
            // This is a capacity refusal, so no syscall is supposed to fail.
            strategy_entry("nopartial-capacity-second-cohort", true,
                           cr257::kNaN, cr257::kNaN, 1.0);
        } else if (fault == Fault::callback || fault == Fault::stream_file) {
            throw std::runtime_error("perf-nopartial strategy callback threw");
        } else if (fault == Fault::sink_record) {
            arm(Operation::write, true);
            trace(std::string(3 * cr257::kCacheBytes, 'x'), 17.0);
        } else {
            auto& context = sd::exit_scratch(cr257::adapter(*this));
            injection.capture_command = true;
            {
                sd::ExitScratchRows<std::string> rows(context);
                // More than the real backing cache. read(0,...) must miss at
                // least once even if a final leaf happened to remain cached.
                const std::string seed(3 * cr257::kCacheBytes, 's');
                rows.push_back(seed);
                require(rows.size() == 1 && context.store.record_count() > 0
                        && injection.command_fd >= 0,
                        "SETUP: commit real exit-scratch rows before arming");
                injection.capture_command = false;
                if (fault == Fault::read) {
                    arm(Operation::read);
                    std::vector<std::byte> bytes(seed.size());
                    const auto text = rows.front();
                    sd::exit_scratch_check(text.read(0, {bytes.data(), bytes.size()}), context.store);
                } else if (fault == Fault::write) {
                    arm(Operation::write);
                    rows.push_back(std::string(3 * cr257::kCacheBytes, 'w'));
                } else if (fault == Fault::sync) {
                    sd::exit_scratch_check(context.store.begin(), context.store);
                    arm(Operation::sync);
                    sd::exit_scratch_check(context.store.commit(pd::ScratchDurability::data),
                                           context.store);
                } else {
                    arm(Operation::write);
                    injection.in_scope_release = true;
                    // The real production destructor releases the version
                    // scope and its rows. No local teardown implementation.
                }
            }
            injection.in_scope_release = false;
        }
        throw std::runtime_error("SETUP: intended production fault did not throw");
    }
    void on_source_bar(const pineforge::Bar& bar) override {
        const auto i = callbacks++;
        require(i < driver.signals.size()
                && bar.timestamp == driver.bars[static_cast<std::size_t>(i)].timestamp,
                "SETUP: strategy skipped or duplicated driver input");
        if (i == 0) {
            output_run_begin();
            output_constant(0, 42.0);
        }
        output_bar(bar.timestamp, bar.timestamp + INT64_C(60000));
        output_value(0, bar.close);
        output_event(0, bar.close, "nopartial-output");
        const auto& signal = driver.signals[static_cast<std::size_t>(i)];
        if (signal.entry)
            strategy_entry(cr257::kEntryId, signal.is_long, cr257::kNaN, cr257::kNaN, 1.0,
                           "X257 entry");
        if (signal.bracket)
            strategy_exit(cr257::kExitId, cr257::kEntryId, signal.limit, signal.stop,
                          cr257::kNaN, cr257::kNaN, cr257::kNaN, 100.0, "X257 exit");
        trace("nopartial-prefix", static_cast<double>(i));
        if (fault == Fault::stream_file && i == cr257::kWarmup + 1)
            // Force a physical whole-window prefix well before the fault.
            trace(std::string(3 * cr257::kCacheBytes, 'p'), static_cast<double>(i));
        if (i + 1 != driver.bars.size() || fault == Fault::sink_flush) return;
        rows_before_fault = closed_trade_count();
        require(snapshot_observed && rows_before_fault > 0
                && rows_before_fault == driver.trades.size(),
                "SETUP: real report/trades must precede the fault");
        ++raises;
        try {
            perform_fault();
        } catch (const std::exception& error) {
            caught_origin = true;
            const auto failure = pineforge::classify_run_failure(error);
            origin = failure.code;
            origin_args = failure.args ? *failure.args : "{}";
            throw; // The native consumer, not the test, latches the run failure.
        }
    }
};

void witness_prefix(FaultHost& host, bool sink) {
    Owned prior;
    const int rc = strategy_stream_fill_report(static_cast<BacktestEngine*>(&host),
                                               reinterpret_cast<pf_report_t*>(&prior.value));
    require(rc == 0 && prior.value.total_trades > 0
            && prior.value.input_bars_processed > 0 && prior.value.script_bars_processed > 0,
            "SETUP: live snapshot must already contain real rows");
    if (!sink)
        require(prior.value.trades_len > 0 && prior.value.equity_curve_len > 0
                && prior.value.trace_len > 0 && prior.value.broker_state_hash_len > 0,
                "SETUP: exercise trade/equity/trace/hash arrays before fault");
    require(host.output_bars_len() > 0 && host.output_events_len() > 0,
            "SETUP: recorded output rows/events must precede the fault");
    if (sink) {
        pf_report_sink_receipt_v1 receipt{sizeof receipt, 1};
        require(host.report_sink_receipt_v1(&receipt) == 0 && !receipt.complete
                && receipt.count_t > 0 && receipt.count_h > 0 && receipt.count_x > 0,
                "SETUP: actual sink records must precede the fault");
    }
    host.snapshot_observed = true;
}
}

bool report_value_is_empty(const void* report) {
    ReportC local{};
    std::memcpy(&local, report, sizeof local);
    return empty(local);
}

void expect_empty_readers(void* engine_handle, bool with_sink) {
    auto& host = *static_cast<BacktestEngine*>(engine_handle);
    const auto code = pineforge::run_failure_value_of(host);
    const std::string args = pineforge::run_failure_args_of(host);
    const std::string message = host.last_error();
    std::string leaks;
    const auto check = [&](const char* name, auto reader) {
        Owned out;
        try {
            reader(out.value);
            if (!empty(out.value)) { leaks += name; leaks += " "; }
        } catch (...) {
            leaks += name; leaks += "(threw) ";
        }
    };
    // Gather every read before the final assertion, so the base-tree lab log
    // identifies all leaking readers, rather than stopping at fill_report.
    for (int repeat = 0; repeat != 2; ++repeat) {
        check("fill_report", [&](ReportC& r) { host.fill_report(&r); });
        check("strategy_stream_fill_report", [&](ReportC& r) {
            require(strategy_stream_fill_report(engine_handle,
                    reinterpret_cast<pf_report_t*>(&r)) == 0, "stream report read failed");
        });
        check("native_module::publish_report", [&](ReportC& r) {
            pineforge::native_module::publish_report(&host, reinterpret_cast<pf_report_t*>(&r));
        });
        pineforge::ReportEnvelopeV1 envelope{};
        envelope.metadata.struct_size = sizeof envelope.metadata;
        envelope.metadata.version = PF_REPORT_ENVELOPE_VERSION;
        try {
            const auto rc = host.fill_report_envelope_v1(&envelope);
            // A failed realtime stream currently leaves stream_phase_ at
            // REALTIME, so the envelope can refuse BUSY without writing.
            // An empty caller-owned destination must stay empty in that case.
            if ((rc != PF_REPORT_MODE_ACCEPTED && rc != PF_REPORT_MODE_BUSY
                    && !(with_sink && rc == PF_REPORT_MODE_CONFLICT))
                || !empty(envelope.report)
                || envelope.metadata.complete != 0)
                leaks += "fill_report_envelope_v1 ";
            // The sink GUARD pins the existing ReportC contract. Logical sink
            // prefix counters are a separate audited leak (see DESIGN.md).
            if (!with_sink && (envelope.metadata.trades.logical_count
                    || envelope.metadata.equity_curve.logical_count
                    || envelope.metadata.trace.logical_count
                    || envelope.metadata.trace_names.logical_count
                    || envelope.metadata.broker_state_hash.logical_count
                    || envelope.metadata.output_rows.logical_count
                    || envelope.metadata.output_events.logical_count))
                leaks += "envelope-logical-counts ";
        } catch (...) { leaks += "fill_report_envelope_v1(threw) "; }
        BacktestEngine::free_report(&envelope.report);
    }
    pf_report_sink_receipt_v1 receipt{sizeof receipt, 1};
    pf_report_sink_receipt_v1 c_receipt{sizeof c_receipt, 1};
    require(host.report_sink_receipt_v1(&receipt) == 0 && !receipt.complete
            && receipt.sha256_hex[0] == '\0'
            && strategy_report_sink_receipt_v1(engine_handle, &c_receipt) == 0
            && !c_receipt.complete && c_receipt.sha256_hex[0] == '\0',
            "failed run completed its sink receipt");
    pf_report_sink_summary_v1 summary{sizeof summary, 1};
    require(strategy_report_sink_summary_v1(engine_handle, &summary) == 0 && !summary.complete,
            "failed run completed its sink summary");
    if (!with_sink) {
        // The report-indexed C ABI has independent entry points; clearing a
        // ReportC alone must not leave its original rows readable through them.
        if (strategy_closed_trade_entry_id(engine_handle, 0)
            || strategy_closed_trade_exit_id(engine_handle, 0)
            || strategy_closed_trade_exit_comment(engine_handle, 0)
            || strategy_closed_trade_entry_incarnation(engine_handle, 0) != 0
            || strategy_closed_trade_close_cause(engine_handle, 0) != -1)
            leaks += "C-ABI-report-indexed-trade-readers ";
        if (strategy_outputs_bars_len(engine_handle) != 0
            || strategy_outputs_events_len(engine_handle) != 0
            || strategy_outputs_series_count(engine_handle) != 0)
            leaks += "C-ABI-output-counts ";
        std::int64_t open_ms = -7, close_ms = -11, times_written = 0, values_written = 0;
        double value = -17.0, constant = -19.0;
        (void)strategy_outputs_bar_times_copy(
            engine_handle, 0, &open_ms, &close_ms, 1, &times_written);
        (void)strategy_outputs_series_copy(engine_handle, 0, 0, &value, 1, &values_written);
        pf_output_event_v1_t event{};
        const auto got_event = strategy_outputs_event_get(engine_handle, 0, &event, sizeof event);
        const auto constants = strategy_outputs_constants_copy(engine_handle, &constant, 1);
        if (times_written != 0 || values_written != 0 || got_event == 0 || constants > 0
            || open_ms != -7 || close_ms != -11 || value != -17.0 || constant != -19.0)
            leaks += "C-ABI-output-copy-readers ";
        const auto witness = host.optional_sequence_summary();
        if (witness.hashes.count || witness.traces.count || witness.rows.count || witness.events.count)
            leaks += "optional_sequence_summary ";
    }
    require(host.last_run_status() != 0 && pineforge::run_failure_value_of(host).code == code.code
            && std::string(pineforge::run_failure_args_of(host)) == args
            && host.last_error() == message,
            "a publication read changed the original typed failure");
    if (!leaks.empty())
        throw std::runtime_error("NO_PARTIAL_REPORT: failed run published rows through " + leaks);
}

void failure_case(Fault fault) {
    const long page = ::sysconf(_SC_PAGESIZE);
    require(page > 0, "SETUP: query platform page size");
    cr257::platform_page = static_cast<std::size_t>(page);
    cr150_w1::Environment directory("PINEFORGE_COMMAND_SCRATCH_DIR", ".");
    cr150_w1::Environment temporary("TMPDIR", ".");
    const bool sink_fault = fault == Fault::sink_record || fault == Fault::sink_flush;
    const bool sink = sink_fault || fault == Fault::stream_file;
    Artifacts artifacts;
    cr257::Driver driver(cr257::kPeriod * 3);
    FaultHost host(driver, fault);
    if (sink) {
        cr150_w1::install_path(host, artifacts.path, artifacts.directory);
    }
    injection = {};
    Disarm disarm;
    cr257::Measure measure;
    require(host.stream_begin(driver.bars.data(), static_cast<int>(cr257::kWarmup), "1", "1"),
            "SETUP: source warmup failed");
    bool success = true;
    for (Count i = cr257::kWarmup; i < driver.bars.size() && success; ++i) {
        success = host.stream_push_bar(driver.bars[static_cast<std::size_t>(i)]);
        if (success && !host.snapshot_observed && host.closed_trade_count() > 0)
            witness_prefix(host, sink);
    }
    if (success) success = host.stream_end();
    if (fault == Fault::sink_flush) {
        require(success && host.snapshot_observed && host.closed_trade_count() > 0
                && host.last_run_status() == 0, "SETUP: execution must finish before late fault");
        host.rows_before_fault = host.closed_trade_count();
        injection = {};
        injection.operation = Operation::sync;
        injection.sink = injection.armed = true;
        Owned out;
        bool caught = false;
        try { host.fill_report(&out.value); }
        catch (const std::exception& error) {
            caught = true;
            host.origin = pineforge::classify_run_failure(error).code;
        }
        require(caught && empty(out.value), "late sink fault must throw and release owned arrays");
    }
    const auto io = measure.finish();
    injection.armed = injection.capture_command = false;
    const bool syscall = fault == Fault::read || fault == Fault::write || fault == Fault::sync
        || fault == Fault::teardown || sink_fault;
    if (syscall) {
        require(injection.failed == 1 && injection.observed_errno == EBADF
                && !io.classification_error, "SETUP: intended real failing syscall not observed");
        if (!sink) {
            const auto& command = io.classes[cr257::class_index(cr257::FileClass::command)];
            const auto errors = fault == Fault::read ? command.pread.errors
                : fault == Fault::sync ? command.fdatasync.errors : command.pwrite.errors;
            require(errors == injection.failed, "SETUP: fault must belong to production command I/O");
        }
        if (fault == Fault::teardown)
            require(injection.failed_in_scope_release, "SETUP: write did not fail in scope teardown");
    } else {
        require(host.raises == 1 && host.caught_origin && injection.failed == 0,
                "SETUP: production refusal/callback exception did not occur exactly once");
    }
    const auto expected = sink_fault ? RunFailureCode::report_sink_failed
        : fault == Fault::capacity ? RunFailureCode::scratch_capacity_exceeded
        : fault == Fault::typed ? RunFailureCode::engine_invariant
        : (fault == Fault::callback || fault == Fault::stream_file)
            ? RunFailureCode::engine_unclassified_error
        : RunFailureCode::scratch_io_failed;
    require(host.snapshot_observed && host.rows_before_fault > 0
            && host.last_run_status() != 0 && host.origin == expected
            && pineforge::run_failure_value_of(host).code == expected,
            "SETUP: real native host must retain the intended typed run failure");
    if (fault != Fault::sink_flush) {
        require(!success && host.native_state().kind == pineforge::NativeLifecycleKind::Failed,
                "failed callback/scratch/sink record returned native success");
        require(std::string(pineforge::run_failure_args_of(host)) == host.origin_args,
                "native run changed the original fault arguments");
    }
    if (!sink && fault != Fault::capacity)
        require(std::string(pineforge::run_failure_args_of(host)) == "{}",
                "unexpected typed failure arguments");
    if (fault == Fault::capacity)
        require(std::string(pineforge::run_failure_args_of(host)) == "{\"resource\":\"cohort\"}",
                "capacity refusal lost its production resource argument");
    if (sink_fault)
        require(std::string(pineforge::run_failure_args_of(host)) == "{\"reason\":\"sink_io\"}",
                "sink fault changed its existing typed reason");
    std::printf("NOPARTIAL witness fault=%d rows_before=%llu real_failed_syscalls=%llu"
                " errno=%d code=%s artifact=%s\n", static_cast<int>(fault),
                static_cast<unsigned long long>(host.rows_before_fault),
                static_cast<unsigned long long>(injection.failed), injection.observed_errno,
                pineforge::run_failure_code_of(host), sink ? artifacts.path.c_str() : "(no sink)");
    bool file_prefix = false;
    if (fault == Fault::stream_file) {
        cr150_w1::Fd file(::open(artifacts.path.c_str(), O_RDONLY | O_CLOEXEC));
        if (file.value >= 0) {
            struct stat info{};
            require(::fstat(file.value, &info) == 0, "SETUP: stat failed sink artifact");
            file_prefix = info.st_size > 0;
            if (file_prefix) {
                std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
                require(::pread(file.value, bytes.data(), bytes.size(), 0)
                            == static_cast<ssize_t>(bytes.size()),
                        "SETUP: read real failed-run sink prefix");
                require(bytes.find("[\"T\",") != std::string::npos
                        && bytes.find("{\"trailer\":") == std::string::npos,
                        "SETUP: disk prefix must contain real trades and no success trailer");
            }
        } else {
            require(errno == ENOENT, "SETUP: failed sink artifact is unreadable");
        }
    }
    try {
        expect_empty_readers(static_cast<BacktestEngine*>(&host), sink);
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(file_prefix ? "NO_PARTIAL_SINK_FILE: " : "")
                                 + error.what());
    }
    require(!file_prefix, "NO_PARTIAL_SINK_FILE: failed run left report records at its public path");
}

void success_case() {
    const long page = ::sysconf(_SC_PAGESIZE);
    require(page > 0, "SETUP: query platform page size");
    cr257::platform_page = static_cast<std::size_t>(page);
    cr150_w1::Environment directory("PINEFORGE_COMMAND_SCRATCH_DIR", ".");
    cr150_w1::Environment temporary("TMPDIR", ".");
    cr257::Driver driver(cr257::kPeriod * 3);
    cr257::TradingHost actual(driver);
    cr257::ReferenceScratch memory;
    cr257::TradingHost reference(driver, &memory);
    require(actual.stream_begin(driver.bars.data(), static_cast<int>(cr257::kWarmup), "1", "1"),
            "GUARD: source warmup failed");
    for (Count i = cr257::kWarmup; i < driver.bars.size(); ++i)
        require(actual.stream_push_bar(driver.bars[static_cast<std::size_t>(i)]),
                "GUARD: source push failed");
    require(actual.stream_end(), "GUARD: source completion failed");
    reference.run(driver.bars.data(), static_cast<int>(driver.bars.size()), "1", "1", false);
    cr257::validate_trading(actual, driver);
    cr257::validate_trading(reference, driver);
    require(cr257::canonical(actual).bytes == cr257::canonical(reference).bytes,
            "GUARD: full report/trade bytes changed against independent memory reference");
    // Separate recording-enabled twins: CR-257's complete canonical encoder
    // intentionally refuses optional arrays, so compare their hash bits here.
    // Match ingress phases for the hash comparison: the continuation includes
    // the stream scheduler's phase, unlike the economic report comparison.
    cr257::TradingHost recorded(driver);
    cr257::ReferenceScratch hash_memory;
    cr257::TradingHost hash_reference(driver, &hash_memory);
    recorded.set_broker_state_hash_recording(true);
    hash_reference.set_broker_state_hash_recording(true);
    require(recorded.stream_begin(driver.bars.data(), static_cast<int>(cr257::kWarmup), "1", "1"),
            "GUARD: recorded warmup failed");
    require(hash_reference.stream_begin(driver.bars.data(),
                static_cast<int>(cr257::kWarmup), "1", "1"),
            "GUARD: independent recorded warmup failed");
    for (Count i = cr257::kWarmup; i < driver.bars.size(); ++i) {
        require(recorded.stream_push_bar(driver.bars[static_cast<std::size_t>(i)]),
                "GUARD: recorded push failed");
        require(hash_reference.stream_push_bar(driver.bars[static_cast<std::size_t>(i)]),
                "GUARD: independent recorded push failed");
    }
    require(recorded.stream_end(), "GUARD: recorded completion failed");
    require(hash_reference.stream_end(), "GUARD: independent recorded completion failed");
    cr257::validate_trading(recorded, driver);
    cr257::validate_trading(hash_reference, driver);
    Owned a, b;
    recorded.fill_report(&a.value); hash_reference.fill_report(&b.value);
    require(a.value.broker_state_hash && b.value.broker_state_hash
            && a.value.broker_state_hash_len == static_cast<std::int64_t>(driver.bars.size())
            && a.value.broker_state_hash_len == b.value.broker_state_hash_len,
            "GUARD: recording-enabled comparison must contain every hash");
    require(std::equal(a.value.broker_state_hash,
                       a.value.broker_state_hash + a.value.broker_state_hash_len,
                       b.value.broker_state_hash)
            && recorded.broker_state_hash() == hash_reference.broker_state_hash()
            && a.value.broker_state_hash[a.value.broker_state_hash_len - 1]
                == recorded.broker_state_hash(),
            "GUARD: broker hash bits changed against independent reference");
    // Readers on the successful handle must still return its complete arrays.
    Owned snapshot, module;
    require(strategy_stream_fill_report(static_cast<BacktestEngine*>(&recorded),
                reinterpret_cast<pf_report_t*>(&snapshot.value)) == 0, "GUARD: C snapshot failed");
    pineforge::native_module::publish_report(&recorded, reinterpret_cast<pf_report_t*>(&module.value));
    require(scalars(snapshot.value).bytes == scalars(a.value).bytes
            && scalars(module.value).bytes == scalars(a.value).bytes,
            "GUARD: successful publication scalars changed");
}
}

extern "C" ssize_t perf_nopartial_pread_delegate(
        int fd, void* data, std::size_t bytes, off_t offset) {
    if (perf_nopartial::inject(fd, perf_nopartial::Operation::read)) {
        const auto result = __real_pread(-1, data, bytes, offset);
        perf_nopartial::observed_failure(result);
        return result;
    }
    return __real_pread(fd, data, bytes, offset);
}
extern "C" ssize_t perf_nopartial_pwrite_delegate(
        int fd, const void* data, std::size_t bytes, off_t offset) {
    if (perf_nopartial::injection.capture_command && bytes == cr257::platform_page
        && perf_nopartial::command_fd(fd))
        perf_nopartial::injection.command_fd = fd;
    if (perf_nopartial::inject(fd, perf_nopartial::Operation::write)) {
        const auto result = __real_pwrite(-1, data, bytes, offset);
        perf_nopartial::observed_failure(result);
        return result;
    }
    return __real_pwrite(fd, data, bytes, offset);
}
extern "C" int perf_nopartial_fdatasync_delegate(int fd) {
    if (perf_nopartial::inject(fd, perf_nopartial::Operation::sync)) {
        const int result = __real_fdatasync(-1);
        perf_nopartial::observed_failure(result);
        return result;
    }
    return __real_fdatasync(fd);
}
