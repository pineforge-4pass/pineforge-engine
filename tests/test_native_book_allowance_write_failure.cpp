#include "book_allowance_core_fixture.hpp"
#include "../src/native_execution_consumer.hpp"
#include <pineforge/detail/scratch_errors.hpp>
#include <pineforge/native_host.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace {
using namespace book_allowance_core;

struct Fault {
    int stage = 0;
    bool overwrite = false;
    bool saw_point = false;
    std::size_t prefix = 0;
    std::uint64_t word = 0;
    std::uint64_t reads = 0;
    std::uint64_t writes = 0;
    unsigned failures = 0;
} fault;
} // namespace

// Linux-only test interposition, with healthy operations forwarded directly to
// the same positional syscall. No production hook or linker option is needed.
extern "C" ssize_t pread(int fd, void* data, std::size_t size, off_t offset) {
    ++fault.reads;
    return static_cast<ssize_t>(::syscall(SYS_pread64, fd, data, size, offset));
}
extern "C" ssize_t pwrite(int fd, const void* data, std::size_t size, off_t offset) {
    ++fault.writes;
    if (fault.stage == 1) {
        fault.stage = 2;
        struct stat info {};
        fault.overwrite = ::fstat(fd, &info) == 0 && offset >= 0
            && static_cast<std::uint64_t>(offset) + size
                <= static_cast<std::uint64_t>(info.st_size);
        fault.prefix = size / 2;
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; fault.word && i + sizeof(fault.word) <= size; ++i) {
            std::uint64_t word = 0;
            std::memcpy(&word, bytes + i, sizeof word);
            if (word == fault.word) {
                fault.saw_point = true;
                // Stop halfway through the new allowance's point ordinal:
                // the remaining half still belongs to the previous row.
                fault.prefix = i + sizeof(word) / 2;
                break;
            }
        }
        return static_cast<ssize_t>(::syscall(SYS_pwrite64, fd, data, fault.prefix, offset));
    }
    if (fault.stage == 2) {
        fault.stage = 0;
        ++fault.failures;
        errno = ENOSPC;
        return -1;
    }
    return static_cast<ssize_t>(::syscall(SYS_pwrite64, fd, data, size, offset));
}

namespace {
class OpeningList final : public no::OpeningReadback {
public:
    explicit OpeningList(no::RunIdentity run) : run_(std::move(run)) {}
    std::uint64_t count() const noexcept override { return 3; }
    pineforge::detail::ScratchStatus opening_at(std::uint64_t index,
                                                no::RequestHandle& out) const noexcept override {
        if (index >= count()) return pineforge::detail::ScratchStatus::not_found;
        out = {run_, index + 1};
        return pineforge::detail::ScratchStatus::ok;
    }
private:
    no::RunIdentity run_;
};

void external_openings_noop() {
    Fixture f;
    const auto handle = f.submit();
    const auto openings = std::make_shared<OpeningList>(f.core.identity());
    auto definition = std::make_shared<no::RequestDefinition>(*f.newest().definition);
    definition->request.intent = no::Reduce{no::ExplicitUnits{2}};
    definition->request.owner = no::BindOpenings{{}, 7, openings};
    f.newest().definition = std::move(definition);
    no::OpeningsClose authority;
    authority.cycle = 7;
    authority.side = no::Side::Long;
    authority.external_openings = openings;
    f.newest().authority = authority;
    f.newest().allowance = no::AllowanceUnits{new_point, 0.75, 0.25};
    const auto expected = f.newest();
    f.submit();
    const auto at = f.locator(handle);
    const auto generation = f.generation();
    const auto epoch = f.epoch();
    no::TargetObservation alive;
    alive.current_position = no::PositionNonflat{7, no::Side::Long};
    std::vector<no::RequestHandle> expected_openings;
    for (std::uint64_t i = 0; i < openings->count(); ++i) {
        no::RequestHandle opening;
        require(openings->opening_at(i, opening) == pineforge::detail::ScratchStatus::ok,
                "external fixture membership is readable");
        expected_openings.push_back(opening);
        alive.openings.push_back({opening, 7, alive.current_position, true});
    }
    fault = {};
    fault.stage = 1; // Any write, including a decode-time freeze, would fail.
    require(!f.refresh(handle, context(), {}, true, at, generation),
            "missing observations refuse the decoded external close");
    require(!f.refresh(handle, context(), alive, true, at, generation),
            "same-point external close is a no-op");
    require(fault.writes == 0 && fault.reads > 0 && fault.stage == 1,
            "false paths decode external membership with zero writes");
    fault.stage = 0;
    require(f.epoch() == epoch && folded(*f.core.hold_live(handle)) == folded(expected),
            "refused external refresh preserves every logical value and epoch");
    fault = {};
    require(f.refresh(handle, context(new_point + 1), alive, true, at, generation),
            "external membership also survives a successful refresh");
    require(fault.writes == 1, "successful external refresh writes only the book blob");
    const auto refreshed = f.core.hold_live(handle);
    require(refreshed != nullptr, "successfully refreshed external close is readable");
    const auto& owner = std::get<no::BindOpenings>(refreshed->request().owner);
    const auto& refreshed_authority = std::get<no::OpeningsClose>(refreshed->authority);
    for (const auto& decoded : {owner.external_openings, refreshed_authority.external_openings}) {
        require(decoded && decoded->count() == expected_openings.size(),
                "successful refresh preserves owner and authority external membership counts");
        for (std::size_t i = 0; i < expected_openings.size(); ++i) {
            no::RequestHandle opening;
            require(decoded->opening_at(i, opening) == pineforge::detail::ScratchStatus::ok
                && opening == expected_openings[i],
                "successful refresh preserves each owner and authority external opening");
        }
    }
}

template<class Read>
void poisoned(Read&& read, bool check_io = true) {
    const auto reads = fault.reads;
    const auto writes = fault.writes;
    bool threw = false;
    try {
        read();
    } catch (const pineforge::detail::AuxiliaryScratchIoError& error) {
        threw = true;
        require(error.run_failure_code() == pineforge::RunFailureCode::scratch_io_failed,
                "poison carries the existing typed scratch failure");
        require(error.code().value() == ENOSPC, "poison preserves the write errno");
        require(std::string(error.what()).find("scratch write") != std::string::npos,
                "poison preserves the failed write operation");
    }
    require(threw, "later spilled read throws the carried write failure");
    require(!check_io || (fault.reads == reads && fault.writes == writes),
            "poison is checked before any further I/O");
}

void partial_core_write() {
    Fixture f;
    const auto handle = f.submit();
    f.newest().allowance = no::AllowanceUnits{old_point, 0.75, 0.25};
    const auto sentinel = f.submit();
    auto held_iterator = f.core.live().begin();
    while (held_iterator->handle() != handle) ++held_iterator;
    const auto at = f.locator(handle);
    const auto generation = f.generation();
    auto token = f.core.prepare_cancel(sentinel, f.ordinal);
    const auto epoch = f.epoch();
    const auto last = f.core.last_ordinal();
    const auto before = f.core.book_refresh_work();
    fault = {};
    fault.word = UINT64_C(0x1234567876543210);
    fault.stage = 1;
    poisoned([&] {
        f.refresh(handle, context(fault.word), {}, true, at, generation);
    }, false);
    require(fault.overwrite && fault.saw_point && fault.failures == 1 && fault.prefix > 0,
            "fault split the spilled allowance overwrite at its point ordinal");
    require(f.epoch() == epoch && f.core.last_ordinal() == last,
            "failed row publication does not publish an epoch or binding ordinal");
    const auto after = f.core.book_refresh_work();
    no_churn(after);
    require(after.in_place_writes == before.in_place_writes
        && after.appended_writes == before.appended_writes
        && after.spilled_refreshes == before.spilled_refreshes,
        "failed overwrite counts no successful refresh");
    poisoned([&] { (void)f.core.hold_live(handle); });
    poisoned([&] { (void)f.core.find_live(handle); });
    poisoned([&] { (void)f.core.live().begin(); });
    poisoned([&] { (void)*held_iterator; });
    poisoned([&] { (void)held_iterator.hold(); });
    poisoned([&] { f.refresh(handle, context(), {}, true, at, generation); });
    poisoned([&] { f.core.rethrow_book_read_failure(); });
    // Even with a resident token, the failed book cannot be used to continue:
    // this test does not install a token against an unreadable run.
    (void)token;
    f.core.reset({"working-book-allowance", 2});
    require(!f.core.book_spilled() && f.core.live_size() == 0,
            "reset discards both the poisoned rows and the carried failure");
    f.core.rethrow_book_read_failure();
}

class FaultHost final : public pineforge::NativeStrategyHost {
public:
    const Core& core() {
        return pineforge::as_native_consumer(execution_consumer()).request_core();
    }
    void on_native_input(const pineforge::Bar&, const pineforge::NativeInputContext&) override {
        if (++inputs == 2) {
            fault = {};
            fault.stage = 1;
        }
    }
    void on_native_bar(const pineforge::Bar&, const pineforge::NativeDecisionContext&) override {
        if (++callbacks != 1) return;
        auto& requests = const_cast<Core&>(core());
        for (std::size_t i = 0; i < resident_rows + 2; ++i) {
            no::Request request{no::Transact{1}, "limit", ""};
            request.trigger = no::Limit{1};
            require(submit(request).handle.has_value(), "fault run admits the full book");
            // Seed the old point while the row is resident so that the next
            // matching refresh must overwrite an equal-width spilled blob.
            (requests.*member(Residents{})).back().allowance =
                no::AllowanceUnits{old_point, 1, 1};
        }
        require(core().book_spilled(), "fault run really has spilled winners");
    }
    int inputs = 0;
    int callbacks = 0;
};

void consumer_failure_boundary() {
    FaultHost host;
    pineforge::NativeRunSpec spec;
    spec.identity = {"working-book-write-failure", 1};
    spec.input_tf = spec.script_tf = "1";
    spec.tickerid = "TEST:ALLOWANCE";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.slot_label_policy = pineforge::NativeSlotLabelPolicy::FeedTolerant;
    spec.initial_capital = 1000000;
    spec.point_value = spec.account_fx = 1;
    spec.price_tick = 0.25;
    require(host.configure_native(spec).status == pineforge::NativeSetupStatus::Applied,
            "fault host configures");
    const pineforge::Bar bars[] = {
        {100, 101, 99, 100, 1, INT64_C(1736121600000)},
        {100, 101, 99, 100, 1, INT64_C(1736121660000)}};
    host.run(bars, 2);
    require(fault.failures == 1 && fault.overwrite, "consumer hit a partial refresh overwrite");
    const auto state = host.native_state();
    require(state.kind == pineforge::NativeLifecycleKind::Failed
        && state.failure.code == pineforge::NativeFailureCode::Allocation
        && state.failure.operation == pineforge::NativeFailureOperation::Settlement
        && state.failure.ordinal != 0, "refresh failure retains Allocation/Settlement/point");
    require(host.callbacks == 1 && host.trade_count() == 0,
            "failure stops before another calculation or trade");
    poisoned([&] { host.core().rethrow_book_read_failure(); });
    poisoned([&] { (void)host.native_working_requests(); });
    // The continuation also folds other stores; those can read before its
    // book fold encounters the poison.
    poisoned([&] { (void)host.native_continuation_hash(); }, false);
}
} // namespace

int main() {
    try {
        external_openings_noop();
        partial_core_write();
        consumer_failure_boundary();
        return 0;
    } catch (const std::exception& error) {
        fault.stage = 0;
        std::fprintf(stderr, "book allowance write failure: %s\n", error.what());
        return 1;
    }
}
