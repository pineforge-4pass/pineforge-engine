// Exercise dependency scratch failures through an automatic native parent match.
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/detail/scratch_store.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/source/exit_scratch.hpp>
#include "../src/native_execution_consumer.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v19 {
struct NativeExecutionConsumerProbe {
    static const detail::CommandScratchStore* dependency(const NativeExecutionConsumer& consumer) {
        return consumer.dependency_scratch_;
    }
    static std::uint64_t queued(const NativeExecutionConsumer& consumer) {
        return consumer.dependency_scratch_stats_.queued_rows;
    }
};
} // inline namespace engine_script_run_v19
} // namespace pineforge

namespace {
using namespace pineforge;
namespace pd = pineforge::detail;
namespace sd = pineforge::source::detail;
using Status = pd::ScratchStatus;

constexpr std::size_t kPageSize = 512;
constexpr std::size_t kBackingPages = 4096;
constexpr std::size_t kDescendants = 64;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr const char* kDependencyEnglish = "dependency scratch contract operation failed";
constexpr const char* kCarriedArgs = R"({"reason":"arena_exhausted","store":"command"})";

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

enum class FaultKind { none, write, read, growth, byte_cap, carried };

const char* row_name(FaultKind kind) {
    switch (kind) {
    case FaultKind::none: return "unarmed_control";
    case FaultKind::write: return "write_fault";
    case FaultKind::read: return "read_fault";
    case FaultKind::growth: return "growth_fault";
    case FaultKind::byte_cap: return "byte_cap_refusal";
    case FaultKind::carried: return "carried_failure";
    }
    throw std::logic_error("unknown scratch fault selection");
}

struct IoCounters {
    std::uint64_t reads = 0, writes = 0, io_errors = 0, contract_errors = 0;
    static void charge(void* context, const pd::ScratchIoCharge& io) noexcept {
        auto& self = *static_cast<IoCounters*>(context);
        if (io.operation == pd::ScratchIoOperation::read_page) ++self.reads;
        if (io.operation == pd::ScratchIoOperation::write_page) ++self.writes;
        if (io.status == Status::io_error) ++self.io_errors;
        if (io.status == Status::programming_error) ++self.contract_errors;
    }
    pd::ScratchIoObserver observer() noexcept { return {this, &IoCounters::charge}; }
};

struct FaultObservation {
    std::uint64_t count = 0;
    pd::ScratchIoOperation operation = pd::ScratchIoOperation::read_page;
    pd::ScratchPageId page = pd::scratch_no_page;
    std::uint64_t extent_before = 0, extent_after = 0;
    std::uint64_t requested_end = 0, byte_cap = 0;
    bool carried = false;
};

// Only the selected backing operation fails, once. Memory remains generously
// provisioned; refusing growth or byte admission never exhausts that memory.
class FaultBacking final : public pd::ScratchBacking {
public:
    FaultBacking(pd::FixedMemoryScratchBacking& inner, pd::ScratchIoObserver observer)
        : ScratchBacking(inner.kind(), inner.page_size(), observer), inner_(inner),
          prepared_(EFBIG, "scratch arena exhausted",
                    {{"reason", "arena_exhausted"}, {"store", "command"}}) {}

    std::uint64_t page_count() const noexcept override { return inner_.page_count(); }
    const pd::ReportScratchIoError* carried_failure() const noexcept override {
        return carrying_ ? &prepared_ : nullptr;
    }
    const pd::ReportScratchIoError& prepared_failure() const noexcept { return prepared_; }
    const FaultObservation& observation() const noexcept { return fault_; }
    std::uint64_t extending_writes() const noexcept { return extending_writes_; }

    void arm(FaultKind kind) {
        require(kind != FaultKind::none && !pending_ && fault_.count == 0,
                "arm one scratch fault after healthy setup");
        kind_ = kind;
        byte_cap_ = inner_.page_count() * static_cast<std::uint64_t>(page_size());
        pending_ = true;
    }

protected:
    Status do_read_page(pd::ScratchPageId id, pd::ScratchBuffer destination) const noexcept override {
        carrying_ = false;
        if (pending_ && kind_ == FaultKind::read)
            return inject(pd::ScratchIoOperation::read_page, id);
        return inner_.read_page(id, destination);
    }

    Status do_write_page(pd::ScratchPageId id, pd::ScratchBytes source) noexcept override {
        carrying_ = false;
        const bool extending = id >= inner_.page_count();
        if (extending) ++extending_writes_;
        const auto end = (id + 1) * static_cast<std::uint64_t>(page_size());
        if (pending_ && (kind_ == FaultKind::write || kind_ == FaultKind::carried
                || (kind_ == FaultKind::growth && extending)
                || (kind_ == FaultKind::byte_cap && extending && end > byte_cap_)))
            return inject(pd::ScratchIoOperation::write_page, id);
        return inner_.write_page(id, source);
    }

    Status do_sync(pd::ScratchDurability durability) noexcept override {
        carrying_ = false;
        return inner_.sync(durability);
    }

private:
    Status inject(pd::ScratchIoOperation operation, pd::ScratchPageId id) const noexcept {
        pending_ = false;
        ++fault_.count;
        fault_.operation = operation;
        fault_.page = id;
        fault_.extent_before = inner_.page_count();
        fault_.requested_end = (id + 1) * static_cast<std::uint64_t>(page_size());
        fault_.byte_cap = byte_cap_;
        carrying_ = kind_ == FaultKind::carried;
        fault_.carried = carrying_;
        // Return before forwarding any bytes or changing the physical extent.
        fault_.extent_after = inner_.page_count();
        return Status::io_error;
    }

    pd::FixedMemoryScratchBacking& inner_;
    pd::ReportScratchIoError prepared_;
    FaultKind kind_ = FaultKind::none;
    std::uint64_t byte_cap_ = 0, extending_writes_ = 0;
    mutable bool pending_ = false, carrying_ = false;
    mutable FaultObservation fault_;
};

struct ScratchFixture {
    std::vector<std::byte> storage;
    pd::FixedMemoryScratchBacking memory;
    IoCounters io;
    FaultBacking backing;
    pd::CommandScratchStore store;

    explicit ScratchFixture(pd::ScratchKeyOrder order)
        : storage(kBackingPages * kPageSize),
          memory({storage.data(), storage.size()}, kPageSize),
          backing(memory, io.observer()),
          store({2, kPageSize, 1, 1, order, pd::ScratchOpenMode::create_empty}, &backing) {
        require(memory.status() == Status::ok && store.status() == Status::ok,
                "open healthy scratch storage");
        require(store.record_count() == 0 && !store.transaction_active()
                && store.carried_failure() == nullptr && backing.carried_failure() == nullptr,
                "scratch starts empty, inactive and without a carried failure");
    }
};

NativeRunSpec run_spec() {
    NativeRunSpec spec;
    spec.identity = {"dependency-scratch-storage", 1};
    spec.input_tf = spec.script_tf = "1";
    spec.tickerid = "TEST:SCRATCH";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.price_tick = 0.25;
    spec.initial_capital = 100000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.close_execution = NativeCloseExecution::NextEligiblePoint;
    spec.event_retention = NativeEventRetention::Full;
    spec.report_policy = NativeReportPolicy::KernelRecorded;
    return spec;
}

const std::array<Bar, 3> kBars{{
    {100.0, 100.0, 100.0, 100.0, 1.0, 1704067200000LL},
    {101.0, 101.0, 101.0, 101.0, 1.0, 1704067260000LL},
    {102.0, 102.0, 102.0, 102.0, 1.0, 1704067320000LL},
}};

void require_healthy(const NativeStrategyHost& host) {
    if (host.last_run_status() != 0 || !host.last_error().empty())
        throw coded<std::logic_error>(run_failure_value_of(host), host.last_error());
}

std::string failure_details(const NativeStrategyHost& host) {
    return std::string("code=") + run_failure_code_of(host)
        + " args=" + run_failure_args_of(host) + " english=\"" + host.last_error() + "\"";
}

void print_observation(const char* row, const NativeStrategyHost& host,
                       const ScratchFixture& fixture, std::uint64_t queued = 0) {
    const auto& fault = fixture.backing.observation();
    const char* operation = fault.count == 0 ? "none"
        : fault.operation == pd::ScratchIoOperation::read_page ? "read_page" : "write_page";
    std::fprintf(stderr, "%s: status=%d lifecycle=%u %s args_null=%s\n", row,
        host.last_run_status(), static_cast<unsigned>(host.native_state().kind),
        failure_details(host).c_str(), run_failure_value_of(host).args ? "false" : "true");
    std::fprintf(stderr,
        "  reads=%llu writes=%llu queued=%llu backing_pages=%llu extending_writes=%llu "
        "faults=%llu operation=%s page=%llu "
        "extent_before=%llu extent_after=%llu requested_end=%llu byte_cap=%llu carried=%s\n",
        static_cast<unsigned long long>(fixture.io.reads),
        static_cast<unsigned long long>(fixture.io.writes),
        static_cast<unsigned long long>(queued),
        static_cast<unsigned long long>(fixture.memory.page_count()),
        static_cast<unsigned long long>(fixture.backing.extending_writes()),
        static_cast<unsigned long long>(fault.count), operation,
        static_cast<unsigned long long>(fault.page),
        static_cast<unsigned long long>(fault.extent_before),
        static_cast<unsigned long long>(fault.extent_after),
        static_cast<unsigned long long>(fault.requested_end),
        static_cast<unsigned long long>(fault.byte_cap), fault.carried ? "true" : "false");
}

void require_code(const NativeStrategyHost& host, RunFailureCode expected) {
    if (run_failure_value_of(host).code != expected
            || std::string(run_failure_code_of(host)) != run_failure_code_name(expected))
        throw std::runtime_error(std::string("expected code=") + run_failure_code_name(expected)
                                 + "; actual " + failure_details(host));
}

void require_plain_failure(const NativeStrategyHost& host, const char* english) {
    require(host.last_run_status() == 1, "scratch I/O failure must set run status 1");
    require(host.native_state().kind == NativeLifecycleKind::Failed,
            "scratch I/O failure must leave a Failed lifecycle");
    require(host.last_error() == english, "scratch I/O failure must retain its exact English");
    require(run_failure_value_of(host).args == nullptr,
            "status-only scratch I/O failure must have null arguments");
    require(std::string(run_failure_args_of(host)) == "{}",
            "status-only scratch I/O failure must expose the empty argument object");
    require_code(host, RunFailureCode::scratch_io_failed);
}

class DependencyHost final : public NativeStrategyHost {
public:
    DependencyHost(ScratchFixture& fixture, FaultKind kind) : fixture_(fixture), kind_(kind) {}

    NativeExecutionConsumer& consumer() { return as_native_consumer(execution_consumer()); }

    void on_native_run_begin() override {
        require_healthy(*this);
        require(physical_position().signed_units == 0.0 && native_open_lots(kNa).empty()
                && native_working_requests().empty(), "native setup begins without positions or requests");
        const auto* provisioned = NativeExecutionConsumerProbe::dependency(consumer());
        require(provisioned != nullptr && provisioned != &fixture_.store
                && provisioned->status() == Status::ok && provisioned->record_count() == 0
                && !provisioned->transaction_active(),
                "production dependency scratch is provisioned before the callback");
        consumer().set_dependency_scratch(fixture_.store);
        require(NativeExecutionConsumerProbe::dependency(consumer()) == &fixture_.store,
                "bind the healthy borrowed dependency store after production provisioning");
        bound = true;
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        namespace no = native_order;
        ++calculations;
        if (calculations == 1) {
            execute(no::Request{no::Transact{1.0}, "long", ""});
            require(physical_position().signed_units > 0.0, "the first execution opens long");
            return;
        }
        if (calculations == 2) {
            execute(no::Request{no::Transact{-2.0}, "reverse", ""});
            require(physical_position().signed_units < 0.0, "the next execution reverses to short");
            const auto parent = put(no::Request{no::Flatten{}, "parent", ""});
            no::Request child{no::Reduce{no::ExplicitUnits{1.0}}, "child", ""};
            child.owner = no::WaitForApplied{parent};
            const auto child_handle = put(child);
            // Automatic matching of the close-only parent retires its child;
            // all these waiting descendants then pass through dependency scratch.
            for (std::size_t index = 0; index < kDescendants; ++index) {
                no::Request descendant{no::Transact{1.0}, "descendant-" + std::to_string(index), ""};
                descendant.owner = no::WaitForApplied{child_handle};
                put(descendant);
            }
            require_healthy(*this);
            require(bound && accepted == kDescendants + 4,
                    "accept the complete native request population before injection");
            require(NativeExecutionConsumerProbe::dependency(consumer()) == &fixture_.store
                    && fixture_.store.status() == Status::ok && fixture_.store.record_count() == 0
                    && !fixture_.store.transaction_active() && fixture_.store.carried_failure() == nullptr
                    && fixture_.backing.observation().count == 0 && fixture_.io.io_errors == 0
                    && fixture_.io.contract_errors == 0,
                    "dependency storage remains healthy and inactive through setup");
            before_drain = fixture_.io;
            setup_complete = true;
            if (kind_ != FaultKind::none) fixture_.backing.arm(kind_);
            return;
        }
        require(calculations == 3, "the cascade uses exactly three native calculations");
        require_healthy(*this);
        require(physical_position().signed_units == 0.0 && native_working_requests().empty(),
                "the automatic parent match retires every waiting request");
        queued = NativeExecutionConsumerProbe::queued(consumer());
        require(queued != 0, "the real dependency queue processes waiting descendants");
        require(fixture_.store.record_count() == 0 && !fixture_.store.transaction_active(),
                "the completed drain restores empty, inactive dependency scratch");
        drain_complete = true;
    }

    IoCounters before_drain;
    std::size_t accepted = 0, calculations = 0;
    std::uint64_t queued = 0;
    bool bound = false, setup_complete = false, drain_complete = false;

private:
    native_order::RequestHandle put(const native_order::Request& request) {
        const auto result = submit(request);
        require(result.status == native_order::SubmitStatus::Accepted && result.handle,
                "native scratch fixture request is accepted");
        ++accepted;
        return *result.handle;
    }
    void execute(const native_order::Request& request) {
        const auto result = execute_current({put(request), NativeCurrentPriceRule::AsPresented});
        require_healthy(*this);
        require(std::holds_alternative<native_order::ExecutionAppliedEvent>(result),
                "native current-price setup execution applies");
    }

    ScratchFixture& fixture_;
    FaultKind kind_;
};

void dependency_run(FaultKind kind) {
    // The store and both backing layers outlive the host's borrowed pointer.
    ScratchFixture fixture(NativeExecutionConsumer::dependency_scratch_order());
    DependencyHost host(fixture, kind);
    require(host.configure_native(run_spec()).status == NativeSetupStatus::Applied,
            "configure the native dependency scratch run");
    host.run(kBars.data(), static_cast<int>(kBars.size()));
    print_observation(row_name(kind), host, fixture,
                      NativeExecutionConsumerProbe::queued(host.consumer()));
    require(host.bound && host.setup_complete && host.accepted == kDescendants + 4,
            "dependency scratch failures must occur after successful native setup");
    require(fixture.io.contract_errors == 0, "fixture memory capacity must never be exhausted");
    if (kind == FaultKind::none) {
        require_healthy(host);
        require(host.native_state().kind == NativeLifecycleKind::Completed,
                "the unarmed native run completes");
        require(host.drain_complete && host.queued != 0 && host.calculations == 3,
                "the unarmed run reaches and completes the dependency queue");
        require(fixture.io.reads > host.before_drain.reads
                && fixture.io.writes > host.before_drain.writes,
                "the unarmed dependency drain actually reads and writes backing pages");
        require(fixture.memory.page_count() > 4 && fixture.backing.extending_writes() != 0
                && fixture.memory.page_count() < fixture.memory.capacity_pages(),
                "dependency data exceeds the two-page cache with spare backing capacity");
        require(fixture.backing.observation().count == 0 && fixture.io.io_errors == 0,
                "the unarmed control has no storage fault");
        require(fixture.store.record_count() == 0 && !fixture.store.transaction_active(),
                "unarmed completion leaves the borrowed scratch store empty and inactive");
        return;
    }

    const auto& fault = fixture.backing.observation();
    require(fault.count == 1 && fixture.io.io_errors == 1,
            "the real dependency drain reaches exactly one injected backing I/O failure");
    require(fault.page < fixture.memory.capacity_pages(),
            "the faulted page fits the generously provisioned memory backing");
    require(fault.operation == (kind == FaultKind::read ? pd::ScratchIoOperation::read_page
                                                       : pd::ScratchIoOperation::write_page),
            "the selected backing operation is the one that fails");
    require(host.calculations == 2 && !host.drain_complete,
            "automatic dependency failure stops the run before the third calculation");
    if (kind == FaultKind::read)
        require(fault.page < fault.extent_before && fixture.io.reads > host.before_drain.reads,
                "the selected dependency read reaches an existing backing page");
    if (kind == FaultKind::growth || kind == FaultKind::byte_cap) {
        require(fault.page >= fault.extent_before && fault.extent_after == fault.extent_before
                && fixture.memory.page_count() == fault.extent_before,
                "refuse an extending write before changing the physical extent");
        if (kind == FaultKind::byte_cap)
            require(fault.requested_end > fault.byte_cap,
                    "the dependency extending write exceeds the admitted byte cap");
    }
    if (kind == FaultKind::carried) {
        require(fault.carried, "the selected dependency write carries its prepared exception");
        require(host.last_run_status() == 1 && host.native_state().kind == NativeLifecycleKind::Failed,
                "a carried dependency I/O failure leaves a failed run");
        require_code(host, RunFailureCode::report_sink_failed);
        const auto value = run_failure_value_of(host);
        require(value.args != nullptr && *value.args == kCarriedArgs
                && std::string(run_failure_args_of(host)) == kCarriedArgs,
                "the carried dependency failure preserves the exact reason and store arguments");
        require(host.last_error() == fixture.backing.prepared_failure().what(),
                "the carried dependency failure preserves the exact exception English");
        const auto* retained = fixture.store.carried_failure();
        require(retained != nullptr && retained->run_failure_code() == RunFailureCode::report_sink_failed
                && retained->code() == std::error_code(EFBIG, std::generic_category())
                && retained->run_failure().args != nullptr
                && *retained->run_failure().args == kCarriedArgs
                && std::string(retained->what()) == fixture.backing.prepared_failure().what(),
                "the dependency store retains the original typed exception and errno");
        return;
    }
    require(!fault.carried && fixture.store.carried_failure() == nullptr,
            "the injected dependency I/O status has no carried exception");
    require_plain_failure(host, kDependencyEnglish);
}

class ExitHost final : public NativeStrategyHost {
public:
    ExitHost(ScratchFixture& fixture, sd::ExitScratchContext& context, bool inject)
        : fixture_(fixture), context_(context), inject_(inject) {}

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        require_healthy(*this);
        require(fixture_.store.status() == Status::ok && fixture_.store.record_count() == 0
                && !fixture_.store.transaction_active(), "exit scratch setup is healthy and empty");
        sd::ExitScratchIndex index(context_);
        setup_complete = true;
        if (inject_) fixture_.backing.arm(FaultKind::write);
        index.put(7, 11);
        require(index.find(7) == 11, "the unarmed sibling index stores and reads a real row");
        put_complete = true;
    }

    bool setup_complete = false, put_complete = false;

private:
    ScratchFixture& fixture_;
    sd::ExitScratchContext& context_;
    bool inject_;
};

void exit_scratch_run(bool inject) {
    ScratchFixture fixture(sd::exit_scratch_key_order());
    sd::ExitScratchContext context{fixture.store, {}};
    ExitHost host(fixture, context, inject);
    require(host.configure_native(run_spec()).status == NativeSetupStatus::Applied,
            "configure the native exit scratch sibling run");
    host.run(kBars.data(), 1);
    print_observation(inject ? "exit_scratch_sibling" : "exit_scratch_unarmed", host, fixture);
    require(host.setup_complete && fixture.io.contract_errors == 0,
            "the exit scratch sibling reaches healthy setup with sufficient backing memory");
    if (!inject) {
        require_healthy(host);
        require(host.native_state().kind == NativeLifecycleKind::Completed && host.put_complete,
                "the unarmed sibling run completes a real index put");
        require(fixture.io.writes != 0 && fixture.io.io_errors == 0,
                "the unarmed sibling writes backing storage without a fault");
        require(fixture.store.record_count() == 0 && !fixture.store.transaction_active(),
                "the unarmed sibling releases its scratch scope");
        return;
    }
    const auto& fault = fixture.backing.observation();
    require(fault.count == 1 && fixture.io.io_errors == 1
            && fault.operation == pd::ScratchIoOperation::write_page && !fault.carried
            && !host.put_complete && fixture.store.carried_failure() == nullptr,
            "the sibling index put reaches one status-only backing write failure");
    require_plain_failure(host, "exit scratch operation failed");
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc <= 2, "supply at most one scratch test row");
        const std::string row = argc == 2 ? argv[1] : "unarmed_control";
        if (row == "exit_scratch_sibling") {
            exit_scratch_run(false);
            exit_scratch_run(true);
            return 0;
        }
        FaultKind kind;
        if (row == "unarmed_control") kind = FaultKind::none;
        else if (row == "write_fault") kind = FaultKind::write;
        else if (row == "read_fault") kind = FaultKind::read;
        else if (row == "growth_fault") kind = FaultKind::growth;
        else if (row == "byte_cap_refusal") kind = FaultKind::byte_cap;
        else if (row == "carried_failure") kind = FaultKind::carried;
        else throw std::invalid_argument("unknown scratch test row: " + row);
        dependency_run(FaultKind::none);
        if (kind != FaultKind::none) dependency_run(kind);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_dependency_scratch_failure: %s\n", error.what());
        return 1;
    }
}
