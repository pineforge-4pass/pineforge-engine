// Every population and work formula here is a layout estimate from source,
// not measured. The executable prints the actual observations on the lab.
#include "../src/native_cohort_target_work.hpp"
#include "native_cohort_target_fixture.hpp"

#include <sys/syscall.h>
#include <unistd.h>

namespace target_io {
bool armed = false;
std::uint64_t reads = 0;
struct Window {
    Window() { reads = 0; armed = true; }
    ~Window() { armed = false; }
};
}
// Same transparent syscall observer pattern as the existing dormant-book
// tests. This row needs ZERO product edits or observer-build definitions.
extern "C" ssize_t pread(int fd, void* data, std::size_t bytes, off_t offset) {
    if (target_io::armed) ++target_io::reads;
    return static_cast<ssize_t>(::syscall(SYS_pread64, fd, data, bytes, offset));
}

namespace {
using namespace cohort_target_test;
[[maybe_unused]] std::size_t search_depth(std::size_t rows) {
    std::size_t depth = 0;
    while (rows != 0) { ++depth; rows /= 2; }
    return depth;
}

void isolated(std::size_t requested, bool successors, unsigned seed) {
    Host h;
    std::size_t exercised = 0;
    h.calculation = [&](Host& host) {
        const auto population = populate(host, requested, successors, seed);
        auto& c = host.consumer();
        const auto lots = host.native_open_lots(100);
        const std::size_t L = lots.size();
        const std::size_t Q = c.request_core().live_size();
        require(L == population.openings.size() && L != 0, "empty or incomplete lot fixture");
        require(Q == population.closes.size() && Q == 2 * L, "fixture is not a two-leg book");
        require(c.request_core().issued_incarnations().size() == 1, "issued handles did not coalesce");
        std::size_t rosters = 0, members = 0;
        for (const auto& roster : c.request_core().cohorts()) {
            require(roster.origins.size() == 1, "isolated roster does not have one root");
            ++rosters; members += roster.origins.size();
        }
        require(rosters == L && members == L, "isolated roster population differs from lots");
        std::printf("L=%zu Q=%zu Qc=%zu D=%zu r=1 K=1 I=%zu H=%zu payload_ceiling=%zu\n",
            L, Q, Q, rosters, c.request_core().issued_incarnations().size(),
            population.replacements, payload_bytes);
        if (requested > 2048) require(c.request_core().book_spilled(), "fixture did not spill the book");
        for (const auto& opening : population.openings)
            require(c.request_core().find_live(opening) == nullptr, "opening is not terminal");
        std::vector<no::LiveRequest> requests;
        std::vector<no::TargetObservation> expected;
        for (const auto& handle : population.closes) {
            requests.push_back(live(host, handle));
            expected.push_back(reference(host, requests.back()));
            require(expected.back().openings.size() == 1
                    && expected.back().openings.front().has_live_matching_lot,
                    "target witness is empty or does not select one real opening");
        }

        // Independently count one complete canonicalization pass through
        // public membership, in physical order, with actual backing reads.
        // The bound is this measured-on-the-lab input-dependent work, never a
        // pinned literal or a fitted constant. Repeated misses are observable.
        std::uint64_t one_pass_reads = 0;
        {
            target_io::Window window;
            for (const auto& lot : lots) {
                const no::RequestHandle opening{c.request_core().identity(), lot.entry_incarnation};
                (void)c.request_core().cohort_contains(population.cohorts.front(), opening);
            }
            one_pass_reads = target_io::reads;
        }
        Probe::cold(c);
        std::vector<no::TargetObservation> observed;
        observed.reserve(Q);
        const auto lookup_before = c.request_core().book_lookup_reads();
#ifdef PINEFORGE_COHORT_TARGET_OBSERVER
        Counts cold;
#endif
        std::uint64_t reads = 0;
        {
            target_io::Window window;
#ifdef PINEFORGE_COHORT_TARGET_OBSERVER
            work::Scope counter({&cold, Counts::charge});
#endif
            for (const auto& request : requests)
                observed.push_back(Probe::read(c, host, request));
            reads = target_io::reads;
        }
        const auto lookup_after = c.request_core().book_lookup_reads();
        require(observed.size() == Q, "target sweep skipped a close");
        for (std::size_t i = 0; i < Q; ++i)
            require(same(observed[i], expected[i]), "isolated target differs from complete reader");
        std::printf("L=%zu Q=%zu successors=%d root_pass_reads=%llu target_reads=%llu"
                    " lookup_rows=%llu lookup_steps=%llu\n", L, Q, successors,
            static_cast<unsigned long long>(one_pass_reads),
            static_cast<unsigned long long>(reads),
            static_cast<unsigned long long>(lookup_after.rows - lookup_before.rows),
            static_cast<unsigned long long>(lookup_after.steps - lookup_before.steps));
        bool bounded = true;
        if (successors) {
            require(one_pass_reads >= L - 64, "successor backing read witness was not reached");
            require(reads >= L - 64, "cold sweep skipped authoritative root resolution");
            bounded = reads <= one_pass_reads;
            if (!bounded) std::fprintf(stderr,
                "canonical roots were reread across cohort targets: reads=%llu one_pass=%llu\n",
                static_cast<unsigned long long>(reads),
                static_cast<unsigned long long>(one_pass_reads));
        }
#ifdef PINEFORGE_COHORT_TARGET_OBSERVER
        print(cold, L, Q, "cold");
        require(cold.get(work::Event::Target) == Q, "primitive observer missed a target");
        require(cold.get(work::Event::Canonical) >= L, "cold root witness is missing");
        bounded = bounded && cold.get(work::Event::Canonical) <= L
            && cold.get(work::Event::FindLive) == L
            && cold.get(work::Event::Issued) == L
            && cold.get(work::Event::Successor) == L
            && cold.get(work::Event::IssuedComparison)
                <= L * c.request_core().issued_incarnations().size()
            && cold.get(work::Event::ResidentStep) <= L * (search_depth(Q) + 17)
            && cold.get(work::Event::RootDiskRead) <= L * search_depth(population.replacements)
            && cold.get(work::Event::Contains) == 0
            && cold.get(work::Event::RefreshLot) == 0
            && cold.get(work::Event::TargetLot) == 0;
        Counts warm;
        std::vector<no::TargetObservation> warm_observed;
        warm_observed.reserve(Q);
        {
            work::Scope counter({&warm, Counts::charge});
            for (const auto& request : requests)
                warm_observed.push_back(Probe::read(c, host, request));
        }
        require(warm_observed.size() == Q, "warm sweep skipped an observable target");
        for (std::size_t i = 0; i < Q; ++i)
            require(same(warm_observed[i], expected[i]), "warm target differs from the complete reader");
        print(warm, L, Q, "warm");
        require(warm.get(work::Event::Target) == Q, "warm sweep skipped targets");
        bounded = bounded && warm.get(work::Event::Canonical) == 0;
#endif
        require(bounded, "canonical root resolution must be shared across cohort targets");
        exercised = Q;
    };
    // Disk roots are required by the zero-product-edit syscall witness.
    pf_scratch_tables_v1 tables{};
    tables.struct_size = sizeof(tables); tables.version = 1;
    tables.backend = PF_SCRATCH_TABLE_DISK_SCRATCH;
    require(h.set_scratch_tables_v1(&tables) == 0, "disk scratch configuration refused");
    run(h, spec(), tape(1));
    require(exercised == 2 * requested, "isolated sweep did not execute");
}

#ifdef PINEFORGE_COHORT_TARGET_OBSERVER
struct QuietResult {
    Counts work;
    std::vector<std::uint64_t> events;
    std::uint64_t scanned = 0, points = 0, broker = 0, continuation = 0;
    std::size_t lots = 0, requests = 0, fills = 0;
};
QuietResult quiet(std::size_t budget) {
    Host h;
    QuietResult out;
    std::optional<work::Scope> window;
    Population p;
    std::uint64_t before_scans = 0, before_points = 0;
    std::size_t before_fills = 0;
    Probe::budget(h.consumer(), budget);
    h.calculation = [&](Host& host) {
        if (host.bars == 1) p = populate(host, 128, false);
        if (host.bars == 2) {
            out.lots = host.native_open_lots(100).size();
            out.requests = host.consumer().request_core().live_size();
            require(out.lots == p.openings.size() && out.requests == 2 * out.lots,
                    "quiet matcher population is not the real two-leg book");
            Probe::cold(host.consumer());
            before_scans = host.consumer().scanned_rows();
            before_points = host.consumer().fast_points() + host.consumer().eager_points();
            before_fills = host.fills;
            window.emplace(work::Observer{&out.work, Counts::charge});
        }
        if (host.bars == 6) {
            window.reset();
            out.scanned = host.consumer().scanned_rows() - before_scans;
            out.points = host.consumer().fast_points() + host.consumer().eager_points() - before_points;
            out.fills = host.fills - before_fills;
            for (const auto& handle : p.closes) {
                const auto request = live(host, handle);
                judge(host, request);
                require(!reference(host, request).openings.empty(), "quiet matcher target is empty");
            }
        }
    };
    run(h, spec(), tape(6));
    require(!window && out.points > 0 && out.scanned >= out.requests && out.fills == 0,
            "quiet matcher skipped points, skipped requests or traded");
    out.events = event_values(h);
    out.broker = h.broker_state_hash();
    out.continuation = h.native_continuation_hash();
    print(out.work, out.lots, out.requests, "public quiet matcher");
    std::printf("matcher_scans=%llu points=%llu quiet_fills=%zu\n",
        static_cast<unsigned long long>(out.scanned),
        static_cast<unsigned long long>(out.points), out.fills);
    return out;
}
void matcher() {
    const auto original = quiet(0);
    const auto indexed = quiet(payload_bytes);
    require(original.events == indexed.events && original.scanned == indexed.scanned
            && original.points == indexed.points && original.broker == indexed.broker
            && original.continuation == indexed.continuation,
            "matcher reduced work by changing its scans, points, ids or event outputs");
    require(indexed.work.get(work::Event::Target) > indexed.requests,
            "quiet matcher did not execute the repeated target-read chain");
    require(indexed.work.get(work::Event::Canonical) >= indexed.lots,
            "quiet matcher skipped its cold physical roots");
    require(indexed.work.get(work::Event::Canonical) <= indexed.lots,
            "quiet matcher root work exceeded the single populated generation");
}
#endif
}

int main() {
    using namespace cohort_target_test;
#ifdef PINEFORGE_COHORT_TARGET_OBSERVER
    row("resident roots first order", [] { isolated(128, false, 1); });
    row("resident roots second order", [] { isolated(256, false, 7); });
    row("public matcher quiet generation", matcher);
#endif
    row("spilled successor roots without product counters", [] { isolated(2049, true, 3); });
    return failures ? 1 : 0;
}
