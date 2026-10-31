// Bounds below are layout estimates from source, not measured. Heap counters
// and the transparent I/O observer report actual work only when run on the lab.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "../src/native_cohort_target_work.hpp"
#include "native_cohort_target_fixture.hpp"

#include <pineforge/detail/scratch_errors.hpp>
#include <cerrno>
#include <sys/syscall.h>
#include <unistd.h>

namespace backing_fault {
bool once = false;
std::size_t injected = 0;
}
extern "C" ssize_t pread(int fd, void* data, std::size_t bytes, off_t offset) {
    if (backing_fault::once) {
        backing_fault::once = false;
        ++backing_fault::injected;
        errno = EIO;
        return -1;
    }
    return static_cast<ssize_t>(::syscall(SYS_pread64, fd, data, bytes, offset));
}

namespace {
using namespace cohort_target_test;

void budgets_and_retained_capacity() {
    Host h;
    std::size_t checked = 0;
    h.calculation = [&](Host& host) {
        const auto p = populate(host, 7, true);
        const auto request = live(host, p.closes.front());
        const auto expected = reference(host, request);
        require(expected.openings.size() == 1, "budget row has no target witness");
        for (const auto budget : {std::size_t{0}, std::size_t{1}, std::size_t{16},
                                  payload_bytes, payload_bytes * 2}) {
            Probe::budget(host.consumer(), budget);
#ifdef PINEFORGE_COHORT_TARGET_OBSERVER
            Counts counts;
            no::TargetObservation actual;
            {
                work::Scope counter({&counts, Counts::charge});
                actual = Probe::read(host.consumer(), host, request);
            }
            const auto L = host.native_open_lots(100).size();
            require(counts.get(work::Event::Canonical) == L, "budget read skipped root work");
            require(counts.get(work::Event::Contains) == (budget < L * 16 ? L : 0),
                    "budget bypass did not use the complete reader");
#else
            const auto actual = Probe::read(host.consumer(), host, request);
#endif
            require(same(actual, expected), "budget changed complete target bytes");
            require(Probe::capacity_bytes(host.consumer()) <= payload_bytes,
                    "optional index exceeded its fixed payload ceiling");
            ++checked;
        }
        Probe::budget(host.consumer(), payload_bytes);
        judge(host, request);
        const auto capacity = Probe::capacity_bytes(host.consumer());
        struct Recording {
            Recording() { global_allocation::recording = true; }
            ~Recording() { global_allocation::recording = false; }
        } recording;
        const auto retained = global_allocation::live_bytes;
        for (std::size_t i = 0; i < 256; ++i) {
            Probe::cold(host.consumer());
            {
                const auto actual = Probe::read(host.consumer(), host, request);
                require(same(actual, expected), "repeated invalidation changed the target");
            }
            require(Probe::capacity_bytes(host.consumer()) == capacity,
                    "invalidation released or grew the fixed index capacity");
            require(global_allocation::live_bytes == retained,
                    "derived index retained heap with repeated generations");
        }
    };
    run(h, spec(), tape(1));
    require(checked == 5, "budget matrix did not execute");
}

void allocation_bypass() {
    Host h;
    std::size_t checked = 0;
    h.calculation = [&](Host& host) {
        const auto p = populate(host, 6, true);
        const auto request = live(host, p.closes.front());
        const auto expected = reference(host, request);
        Probe::cold(host.consumer());
#ifdef PINEFORGE_HAS_COHORT_OPENING_INDEX
        // A fresh consumer has never reserved an index. The only allocation
        // before fallback is the optional reserve; fail it exactly once.
        const auto refused_before = global_allocation::refused;
        global_allocation::fail_after = 0;
        no::TargetObservation actual;
        try {
            actual = Probe::read(host.consumer(), host, request);
        } catch (...) {
            global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
            throw;
        }
        global_allocation::fail_after = std::numeric_limits<std::size_t>::max();
        require(global_allocation::refused == refused_before + 1,
                "optional allocation fault did not fire");
        require(!Probe::valid(host.consumer()), "failed allocation published an index");
#else
        // No optional allocation exists on the base; still run the full
        // target equality. The fault is a fixed-header extension check.
        const auto actual = Probe::read(host.consumer(), host, request);
#endif
        require(same(actual, expected) && !actual.openings.empty(),
                "allocation bypass changed or emptied the authoritative target");
        ++checked;
    };
    run(h, spec(), tape(1));
    require(checked == 1, "allocation bypass was not reached");
}

void oversized_population() {
    Host h;
    std::size_t checked = 0;
    h.calculation = [&](Host& host) {
        const auto cohort = host.cohort_open();
        // Derive the population from the fixed pair payload, not a lowered
        // production bound. All openings fill through the public entry point.
        const std::size_t n = payload_bytes / sizeof(std::pair<std::uint64_t, std::uint64_t>) + 1;
        for (std::size_t i = 0; i < n; ++i) {
            const auto opening = put(host, {no::Transact{1}, "oversized-" + std::to_string(i), ""});
            host.cohort_add(cohort, opening);
            (void)apply(host, opening);
        }
        const auto request = live(host, put(host, close(host, cohort, "oversized target")));
        const auto expected = reference(host, request);
        const auto L = host.native_open_lots(100).size();
        require(L == n && expected.openings.size() == L, "oversized fixture is empty or incomplete");
        Probe::cold(host.consumer());
#ifdef PINEFORGE_COHORT_TARGET_OBSERVER
        Counts counts;
        no::TargetObservation actual;
        {
            work::Scope counter({&counts, Counts::charge});
            actual = Probe::read(host.consumer(), host, request);
        }
        require(counts.get(work::Event::Contains) == L
                && counts.get(work::Event::TargetLot) == L,
                "oversized view did not use the unchanged complete loop");
#else
        const auto actual = Probe::read(host.consumer(), host, request);
#endif
        require(same(actual, expected), "over-budget fallback lost an opening");
        require(!Probe::valid(host.consumer()), "over-budget partial index was published");
        require(Probe::capacity_bytes(host.consumer()) <= payload_bytes, "index grew with lot population");
        ++checked;
    };
    run(h, spec(), tape(1));
    require(checked == 1, "over-budget row was not executed");
}

void backing_errors() {
    Host h;
    std::size_t checked = 0;
    h.calculation = [&](Host& host) {
        const auto p = populate(host, 70, true);
        const auto request = live(host, p.closes.front());
        const auto expected = reference(host, request);
        require(!expected.openings.empty(), "backing fault has no live successor witness");
        const auto fault = [&](bool indexed) {
            Probe::cold(host.consumer());
            const auto before = backing_fault::injected;
            backing_fault::once = true;
            std::string result;
            try {
                if (indexed) (void)Probe::read(host.consumer(), host, request);
                else (void)reference(host, request);
            } catch (const pineforge::detail::AuxiliaryScratchIoError& error) {
                result = std::to_string(error.code().value()) + ":" + error.what();
            } catch (...) {
                backing_fault::once = false;
                throw;
            }
            backing_fault::once = false;
            require(backing_fault::injected == before + 1, "backing fault was not injected");
            require(!result.empty(), "authoritative backing failure was swallowed");
            return result;
        };
        const auto original = fault(false);
        const auto indexed = fault(true);
        require(indexed == original, "backing failure type, errno or text changed");
        require(!Probe::valid(host.consumer()), "backing failure published a partial index");
        judge(host, request);
        ++checked;
    };
    pf_scratch_tables_v1 tables{};
    tables.struct_size = sizeof(tables); tables.version = 1;
    tables.backend = PF_SCRATCH_TABLE_DISK_SCRATCH;
    require(h.set_scratch_tables_v1(&tables) == 0, "disk roots configuration failed");
    run(h, spec(), tape(1));
    require(checked == 1, "authoritative backing fault row was not reached");
}
}

int main() {
    row("budget matrix and flat retained index", budgets_and_retained_capacity);
    row("optional allocation uses complete fallback", allocation_bypass);
    row("over-budget population uses complete fallback", oversized_population);
    row("authoritative root read errors propagate", backing_errors);
    return failures ? 1 : 0;
}
