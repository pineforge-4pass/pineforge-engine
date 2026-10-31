// The previous header closure must reject set_journal_residency_for_test.
// The independent runtime red uses test_parent_terminal_scratch's
// --journal-limits-only mode, which needs only the previous public API.
#include "../src/native_execution_consumer.hpp"
#include "native_order_full_fold.hpp"

#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/detail/scratch_slab.hpp>
#include <pineforge/detail/scratch_store.hpp>
#include <pineforge/report_sink.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v19 {
struct NativeExecutionConsumerProbe {
    static native_order::WorkingRequestCore& core(NativeExecutionConsumer& consumer) {
        return consumer.requests_;
    }
};
}
}

namespace {
using namespace pineforge;
namespace no = native_order;
namespace pd = detail;
using Bytes = std::vector<std::byte>;
constexpr std::size_t kPage = 32768;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string text(std::size_t size, unsigned salt = 0) {
    std::string result(size, ' ');
    for (std::size_t index = 0; index != size; ++index)
        result[index] = static_cast<char>('a' + (index * 17 + index / kPage + salt) % 26);
    return result;
}

Bytes bytes(const no::CommandEvent& event) {
    const auto source = no::encode_journal_record(event);
    require(source.size() <= std::numeric_limits<std::size_t>::max(),
            "fixture record fits address space");
    Bytes result(static_cast<std::size_t>(source.size()));
    require(source.read(0, {result.data(), result.size()}) == pd::ScratchStatus::ok,
            "complete canonical event bytes");
    return result;
}

struct Snapshot {
    std::vector<Bytes> events;
    std::vector<std::uint64_t> states;
    friend bool operator==(const Snapshot& a, const Snapshot& b) {
        return a.events == b.events && a.states == b.states;
    }
};

void capture(no::WorkingRequestCore& core, Snapshot& out) {
    for (std::size_t position = core.history_base(); position != core.history_end(); ++position) {
        const auto& event = core.history_at(position);
        const auto encoded = bytes(event);
        out.events.push_back(encoded);
        const auto ordinal = std::visit([](const auto& row) { return row.ordinal; }, event);
        const auto* found = core.event_at({core.identity(), ordinal});
        require(found && bytes(*found) == encoded, "ordinal lookup returns every complete event");
    }
    out.states.push_back(full_fold::core_value(core));
}

struct Openings final : no::OpeningReadback {
    no::RunIdentity run;
    std::uint64_t first;
    explicit Openings(no::RunIdentity value, std::uint64_t start = 1)
        : run(std::move(value)), first(start) {}
    std::uint64_t count() const noexcept override { return 4096; }
    pd::ScratchStatus opening_at(std::uint64_t index, no::RequestHandle& out) const noexcept override {
        if (index >= count()) return pd::ScratchStatus::not_found;
        out = {run, first + index};
        return pd::ScratchStatus::ok;
    }
};

struct Observations final : no::OpeningObservationReadback {
    std::shared_ptr<Openings> members;
    explicit Observations(const no::RunIdentity& run)
        : members(std::make_shared<Openings>(run, 10000)) {}
    std::uint64_t count() const noexcept override { return members->count(); }
    pd::ScratchStatus observation_at(std::uint64_t index,
                                    no::OpeningObservation& out) const noexcept override {
        no::RequestHandle handle;
        const auto status = members->opening_at(index, handle);
        if (status == pd::ScratchStatus::ok)
            out = {handle, 7, no::PositionNonflat{7, no::Side::Long}, true};
        return status;
    }
};

pd::ScratchStatus numeric_order(void*, const pd::ScratchByteSource& a,
                               const pd::ScratchByteSource& b, int& order) noexcept {
    if (a.size() != 8 || b.size() != 8) return pd::ScratchStatus::programming_error;
    std::array<std::byte, 8> left{}, right{};
    auto status = a.read(0, {left.data(), left.size()});
    if (status != pd::ScratchStatus::ok) return status;
    status = b.read(0, {right.data(), right.size()});
    if (status != pd::ScratchStatus::ok) return status;
    order = 0;
    for (std::size_t index = left.size(); index-- != 0;) {
        if (left[index] == right[index]) continue;
        order = std::to_integer<unsigned>(left[index]) < std::to_integer<unsigned>(right[index])
            ? -1 : 1;
        break;
    }
    return pd::ScratchStatus::ok;
}

Snapshot core_run(std::size_t maximum, bool direct, bool codec, bool bounded) {
    // This fixed provision belongs to the fixture, never to the journal.
    Bytes medium(codec ? 32 * 1024 * 1024 : 0);
    pd::FixedMemoryScratchBacking backing({medium.data(), medium.size()}, 512);
    std::unique_ptr<pd::CommandScratchStore> store;
    const no::RunIdentity run{"journal-continuations", 1};
    no::WorkingRequestCore core{run};
    if (codec) {
        store = std::make_unique<pd::CommandScratchStore>(
            pd::ScratchStoreConfig{2, 512, 1, 1, {0x6a6f75726e616c01ULL, nullptr, numeric_order},
                                   pd::ScratchOpenMode::create_empty}, &backing);
        require(store->status() == pd::ScratchStatus::ok, "codec fixture store opens");
        no::bind_journal_codec(core, *store);
    } else {
        NativeExecutionConsumer::set_journal_residency_for_test(core, maximum, 0);
    }
    if (bounded) {
        pd::ScratchManifest manifest{};
        manifest.R = 64;
        core.bind_scratch_slab(std::make_shared<pd::ScratchSlab>(manifest));
    }
    std::uint64_t incarnation = 1, ordinal = 1;
    const auto submit = [&](const no::Request& input,
                            const no::CommandContext& context = no::CommandContext{}) {
        no::SubmitResult result;
        if (direct) {
            result = core.apply_submit(input, context, incarnation, ordinal).result;
        } else {
            auto prepared = core.prepare_submit(input, context, incarnation, ordinal);
            auto installed = core.install_submit(std::move(prepared));
            require(std::holds_alternative<no::CommandInstalled<no::SubmitResult>>(installed),
                    "staged submit installs");
            result = std::get<no::CommandInstalled<no::SubmitResult>>(installed).result;
        }
        ++ordinal;
        if (result.status == no::SubmitStatus::Accepted) ++incarnation;
        return result;
    };
    const auto cancel = [&](const no::RequestHandle& handle) {
        if (!direct) return core.cancel(handle, ordinal);
        auto result = core.apply_cancel(handle, ordinal).result;
        ++ordinal;
        return result;
    };
    no::Request huge{no::Transact{1.0}, text(3 * kPage + 17), text(kPage + 41, 3)};
    huge.trigger = no::Limit{50.0};
    const auto first = submit(huge);
    require(first.status == no::SubmitStatus::Accepted && first.handle, "large request is accepted");
    const auto initial_bytes = bytes(core.history_at(0));
    require(initial_bytes.size() > 4 * kPage, "fixture event spans at least five transfer chunks");
    for (unsigned batch = 0; batch != 6; ++batch) {
        no::Request next = huge;
        next.comment = text(2 * kPage + batch + 1, batch + 1);
        no::ReplaceResult replaced;
        if (direct) {
            replaced = core.apply_replace(*first.handle, next, {}, incarnation, ordinal).result;
            ++ordinal;
            if (replaced.status == no::ReplaceStatus::Replaced) ++incarnation;
        } else {
            replaced = core.replace(*first.handle, next, 0, incarnation, ordinal);
        }
        if (batch == 0) {
            require(replaced.status == no::ReplaceStatus::Replaced && replaced.successor,
                    "large replacement is installed");
            require(cancel(*replaced.successor).status == no::CancelStatus::Cancelled,
                    "large replacement is cancelled");
        } else {
            require(replaced.status == no::ReplaceStatus::NotWorking,
                    "old handle remains not working after replacement");
        }
        no::Request refused{no::Transact{0.0}, "external", ""};
        refused.owner = no::BindOpenings{{}, 7, std::make_shared<Openings>(run)};
        require(submit(refused).status == no::SubmitStatus::Rejected,
                "invalid quantity is rejected with the original external openings");
        for (unsigned small = 0; small != 8; ++small) {
            require(submit({no::Transact{0.0}, "small", ""}).status == no::SubmitStatus::Rejected,
                    "small filler is rejected");
        }
    }
    Observations observed{run};
    no::Request close{no::Reduce{no::ExplicitUnits{1.0}}, "bound", ""};
    close.owner = no::BindOpenings{{}, 7, observed.members};
    no::CommandContext context;
    context.external_openings = &observed;
    const auto close_position = core.history_end();
    const auto bound = submit(close, context);
    require(bound.status == no::SubmitStatus::Accepted && bound.handle,
            "large external opening membership is accepted");
    const auto bound_bytes = bytes(core.history_at(close_position));
    require(bound_bytes.size() > kPage, "opening membership alone exceeds one page");
    require(cancel(*bound.handle).status == no::CancelStatus::Cancelled,
            "cancel journals the large OpeningsClose authority");
    require(bytes(core.history_at(close_position)) == bound_bytes,
            "accepted external membership survives continuation readback");
    require(bytes(core.history_at(0)) == initial_bytes, "large event reads back byte for byte");
    {
        const auto& reloaded = std::get<no::AcceptedEvent>(core.history_at(0)).request();
        require(reloaded.label == huge.label && reloaded.comment == huge.comment,
                "continued strings preserve every byte");
    }
    Snapshot result;
    capture(core, result); // more than four pages, including revisits after eviction
    for (std::size_t position = core.history_end(); position-- != core.history_base();)
        require(bytes(core.history_at(position)) == result.events[position],
                "reverse traversal preserves every event after cache eviction");
    require(core.retire_history(2) == 2, "retire through a continued replacement");
    capture(core, result);
    const auto remaining = core.history().size();
    require(core.retire_history(core.last_ordinal()) == remaining, "release all spill pages");
    require(submit(huge).status == no::SubmitStatus::Accepted, "append after releasing pages");
    for (unsigned index = 0; index != 8; ++index)
        (void)submit({no::Transact{0.0}, "reuse", ""});
    capture(core, result); // reuses the spill directory from page zero
    return result;
}

std::vector<int> unlinked_descriptors() {
    std::vector<int> result;
    const long limit = ::sysconf(_SC_OPEN_MAX);
    const long end = limit > 0 ? std::min(limit, 4096L) : 1024L;
    for (int fd = 3; fd < end; ++fd) {
        struct stat status{};
        if (::fstat(fd, &status) == 0 && S_ISREG(status.st_mode) && status.st_nlink == 0)
            result.push_back(fd);
    }
    return result;
}

template <class Operation>
void io_failure(Operation&& operation, const char* message) {
    bool failed = false;
    try {
        operation();
    } catch (const pd::ScratchIoError& error) {
        failed = error.code().value() == EIO
            && error.run_failure_code() == RunFailureCode::report_sink_failed
            && error.run_failure().args && *error.run_failure().args == "{\"reason\":\"scratch_io\"}";
    }
    require(failed, message);
}

void damaged_backing(bool truncated) {
    const auto before = unlinked_descriptors();
    no::WorkingRequestCore core{{"journal-damaged", 1}};
    NativeExecutionConsumer::set_journal_residency_for_test(core, 2, 0);
    std::uint64_t incarnation = 1, ordinal = 1;
    const no::Request huge{no::Transact{0.0}, text(3 * kPage + 17), ""};
    require(core.submit(huge, 0, incarnation, ordinal).status == no::SubmitStatus::Rejected,
            "damaged-page fixture starts with a real large event");
    const auto expected = bytes(core.history_at(0));
    for (unsigned index = 0; index != 32; ++index)
        (void)core.submit({no::Transact{0.0}, "small", ""}, 0, incarnation, ordinal);
    int data_fd = -1;
    struct stat data_status{};
    for (const int fd : unlinked_descriptors()) {
        struct stat status{};
        if (std::find(before.begin(), before.end(), fd) == before.end()
            && ::fstat(fd, &status) == 0 && status.st_size > static_cast<off_t>(3 * kPage)) {
            require(data_fd == -1, "identify exactly one journal payload file");
            data_fd = fd;
            data_status = status;
        }
    }
    require(data_fd != -1, "large event really reached external backing");
    require(bytes(core.history_at(0)) == expected, "seed a successful decoded cache entry");
    for (std::size_t position = 2; position < 26; position += 2)
        (void)core.history_at(position); // evict the seeded entry through distinct pages
    Bytes saved(static_cast<std::size_t>(data_status.st_size));
    require(::pread(data_fd, saved.data(), saved.size(), 0) == static_cast<ssize_t>(saved.size()),
            "save complete backing before the injected fault");
    if (truncated) {
        require(::ftruncate(data_fd, kPage) == 0, "remove every continuation after the first page");
    } else {
        const auto invalid_tag = std::numeric_limits<std::uint32_t>::max();
        require(::pwrite(data_fd, &invalid_tag, sizeof invalid_tag, 0) == sizeof invalid_tag,
                "corrupt the stored event alternative");
    }
    io_failure([&] { (void)core.history_at(0); },
               "cold corrupted/missing continuation returns typed scratch EIO");
    io_failure([&] { (void)core.history_at(0); },
               "failed decode never publishes a cache entry on retry");
    require(::ftruncate(data_fd, data_status.st_size) == 0, "restore backing extent");
    require(::pwrite(data_fd, saved.data(), saved.size(), 0) == static_cast<ssize_t>(saved.size()),
            "restore original backing bytes");
    require(bytes(core.history_at(0)) == expected, "read recovers after backing is repaired");
}

struct FileSizeLimit {
    struct rlimit previous{};
    struct sigaction previous_signal{};
    explicit FileSizeLimit(rlim_t bytes) {
        require(::getrlimit(RLIMIT_FSIZE, &previous) == 0, "read file-size limit");
        require(previous.rlim_max == RLIM_INFINITY || previous.rlim_max >= bytes,
                "fixture file-size limit fits the hard limit");
        struct sigaction ignored{};
        ignored.sa_handler = SIG_IGN;
        sigemptyset(&ignored.sa_mask);
        require(::sigaction(SIGXFSZ, &ignored, &previous_signal) == 0, "ignore fixture SIGXFSZ");
        auto next = previous;
        next.rlim_cur = bytes;
        if (::setrlimit(RLIMIT_FSIZE, &next) != 0) {
            ::sigaction(SIGXFSZ, &previous_signal, nullptr);
            throw std::runtime_error("set fixture file-size limit");
        }
    }
    ~FileSizeLimit() {
        ::setrlimit(RLIMIT_FSIZE, &previous);
        ::sigaction(SIGXFSZ, &previous_signal, nullptr);
    }
};

void interrupted_append() {
    const auto before = unlinked_descriptors();
    no::WorkingRequestCore core{{"journal-interrupted", 1}};
    NativeExecutionConsumer::set_journal_residency_for_test(core, 2, 0);
    std::uint64_t incarnation = 1, ordinal = 1;
    (void)core.submit({no::Transact{0.0}, text(3 * kPage + 17), ""}, 0, incarnation, ordinal);
    (void)core.submit({no::Transact{0.0}, "second", ""}, 0, incarnation, ordinal);
    const auto expected = bytes(core.history_at(0));
    const auto state = full_fold::core_value(core);
    core.reserve(3); // open both extents before installing the real write fault
    {
        FileSizeLimit limit{kPage + 7};
        bool failed = false;
        try {
            (void)core.submit({no::Transact{0.0}, "third", ""}, 0, incarnation, ordinal);
        } catch (const pd::ScratchIoError& error) {
            failed = error.code().value() == EFBIG
                && error.run_failure_code() == RunFailureCode::report_sink_failed
                && error.run_failure().args && *error.run_failure().args == "{\"reason\":\"scratch_io\"}";
        }
        require(failed, "real partial continuation write preserves typed file EFBIG");
    }
    bool partial = false;
    for (const int fd : unlinked_descriptors()) {
        struct stat status{};
        if (std::find(before.begin(), before.end(), fd) == before.end()
            && ::fstat(fd, &status) == 0 && status.st_size == static_cast<off_t>(kPage + 7))
            partial = true;
    }
    require(partial, "OS wrote the first chunk and a partial second chunk");
    require(core.history_end() == 2 && incarnation == 1 && ordinal == 3
                && full_fold::core_value(core) == state,
            "failed continued append publishes no event or counter change");
    require(core.submit({no::Transact{0.0}, "third", ""}, 0, incarnation, ordinal).status
                == no::SubmitStatus::Rejected,
            "retry after backing recovery succeeds");
    require(bytes(core.history_at(0)) == expected && core.history_end() == 3 && ordinal == 4,
            "retry overwrites unpublished chunks and returns the original event");
}

struct TempFile {
    std::string directory = std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp";
    std::string path = directory + "/pineforge-journal-XXXXXX";
    int fd = ::mkstemp(path.data());
    TempFile() { require(fd >= 0, "create fixture file"); }
    ~TempFile() { if (fd >= 0) ::close(fd); ::unlink(path.c_str()); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    std::string read() const {
        struct stat status{};
        require(::fstat(fd, &status) == 0, "stat complete report");
        std::string result(static_cast<std::size_t>(status.st_size), '\0');
        require(::pread(fd, result.data(), result.size(), 0) == static_cast<ssize_t>(result.size()),
                "read complete report bytes");
        return result;
    }
};

void arena_roundtrip() {
    TempFile backing;
    constexpr std::uint64_t arena_bytes = 32 * 1024 * 1024;
    require(::ftruncate(backing.fd, arena_bytes) == 0, "size real arena backing");
    pd::ScratchArena::attach(backing.fd, arena_bytes);
    struct Detach {
        ~Detach() { pd::ScratchArena::detach(); }
    } detach;
    const auto expected = core_run(8192, true, false, true);
    require(core_run(2, true, false, true) == expected,
            "arena and bounded slab route preserves complete continued events");
}

struct TradingHost final : NativeStrategyHost {
    std::size_t maximum;
    std::size_t callbacks = 0;
    explicit TradingHost(std::size_t value) : maximum(value) {}
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++callbacks;
        auto& consumer = as_native_consumer(execution_consumer());
        NativeExecutionConsumer::set_journal_residency_for_test(
            NativeExecutionConsumerProbe::core(consumer), maximum, 0);
        for (unsigned index = 0; index != 4; ++index) {
            execute({no::Transact{1.0}, "open", text(2 * kPage + 17, index)}, true);
            execute({no::Flatten{}, "close", text(kPage + 9, index)}, false);
        }
    }
    void execute(const no::Request& request, bool opening) {
        const auto accepted = submit(request);
        require(accepted.status == no::SubmitStatus::Accepted && accepted.handle,
                "market request accepted at every residency");
        const auto execution = execute_current({*accepted.handle, NativeCurrentPriceRule::AsPresented});
        const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&execution);
        require(applied && applied->terminal && applied->raw_price == 100.0
                    && applied->resolved_price == 100.0
                    && applied->opened_units == (opening ? 1.0 : 0.0)
                    && applied->closed_units == (opening ? 0.0 : 1.0),
                "independently specified fill is unchanged");
    }
    const TradeLedger& rows() const { return trades_; }
    const no::WorkingRequestCore& core() {
        return as_native_consumer(execution_consumer()).request_core();
    }
};

std::pair<Snapshot, std::string> trading_run(std::size_t maximum) {
    TempFile output;
    TradingHost host{maximum};
    NativeRunSpec spec;
    spec.identity = {"journal-trades", 1};
    spec.input_tf = spec.script_tf = "1";
    spec.tickerid = "TEST:JOURNAL";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000;
    spec.point_value = spec.account_fx = 1;
    spec.price_tick = 0.01;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0;
    spec.close_execution = NativeCloseExecution::AfterCalculation;
    spec.event_retention = NativeEventRetention::Full;
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    require(host.configure_native(spec).status == NativeSetupStatus::Applied, "configure trading run");
    const pf_report_sink_v1 sink{sizeof sink, 1, output.path.c_str(), "journal", output.directory.c_str()};
    require(host.set_report_sink_v1(&sink) == 0, "install complete report sink");
    const Bar bar{100, 100, 100, 100, 1, 1736121600000LL};
    host.run(&bar, 1);
    require(host.last_error().empty() && host.native_state().kind == NativeLifecycleKind::Completed
                && host.callbacks == 1 && host.rows().size() == 4,
            "trading run succeeds with four complete trades");
    std::size_t index = 0;
    for (const auto& trade : host.rows()) {
        require(trade.qty == 1.0 && trade.entry_price == 100.0 && trade.exit_price == 100.0
                    && trade.pnl == 0.0 && trade.commission == 0.0
                    && trade.entry_incarnation == 2 * index + 1
                    && trade.entry_comment == text(2 * kPage + 17, index)
                    && trade.exit_comment == text(kPage + 9, index),
                "trade values and continued text match the input model");
        ++index;
    }
    Snapshot snapshot;
    snapshot.states.push_back(host.native_continuation_hash());
    for (const auto& event : host.core().history()) snapshot.events.push_back(bytes(event));
    require(host.native_continuation_hash() == snapshot.states.front(),
            "reading continued history cannot change the continuation hash");
    ReportC report{};
    host.fill_report(&report);
    BacktestEngine::free_report(&report);
    pf_report_sink_receipt_v1 receipt{sizeof receipt, 1};
    require(host.report_sink_receipt_v1(&receipt) == 0 && receipt.complete,
            "report byte comparison covers a complete trailer");
    auto report_bytes = output.read();
    require(!report_bytes.empty(), "report byte comparison is nonempty");
    return {std::move(snapshot), std::move(report_bytes)};
}
}

int main() {
    try {
        for (const bool direct : {false, true}) {
            const auto expected = core_run(8192, direct, false, false);
            require(core_run(2, direct, false, false) == expected,
                    "tiny and large resident windows yield byte-identical complete state");
            require(core_run(2, direct, false, true) == expected,
                    "bounded slab route preserves every event and state");
            require(core_run(2, direct, true, false) == expected,
                    "bound codec spans its 512-byte pages identically");
        }
        damaged_backing(false);
        damaged_backing(true);
        interrupted_append();
        arena_roundtrip();
        require(trading_run(2) == trading_run(8192),
                "tiny and large journals preserve fills, trades, handles, continuation and report bytes");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "journal continuations: %s\n", error.what());
        return 1;
    }
    std::puts("PASS journal continuations: residency, codec, bounded, damaged I/O, trades and reports");
    return 0;
}
