#include "position_tail_retirement_fixture.hpp"

int main() {
    try {
        using namespace o1w_tail;
        // The archive digest's independent history walk is covered by the
        // small differential. This witness counts only production work.
        check_archive = false;
        const int small = 1000;
        const int large = 2000;
        const Work a = scale_run(small, true, false);
        const Work b = scale_run(large, true, false);
        check(a.pairs > 0 && a.placements > 0 && a.families > 0,
              "work counters did not observe real loops");
        check(a.peak_archived > 0 && b.peak_archived > a.peak_archived,
              "completed exit families did not accumulate in the archive");
        // Compare to a small constant multiple, not a literal counter value.
        // Quadratic doubling (~4x) and cubic doubling (~8x) both fail.
        const std::uint64_t multiple = 3;
        check(b.pairs <= multiple * a.pairs, "K3 candidate pairs grew superlinearly");
        check(b.placements <= multiple * a.placements, "hash placement visits grew superlinearly");
        check(b.families <= multiple * a.families, "hash family visits grew superlinearly");
        check(b.settlements <= multiple * a.settlements, "family settlement revisited history");
        std::printf("PF_TAIL_WORK cycles=%d/%d k3_pairs=%llu/%llu "
                    "hash_placements=%llu/%llu hash_families=%llu/%llu "
                    "settle_families=%llu/%llu\n", small, large,
                    static_cast<unsigned long long>(a.pairs),
                    static_cast<unsigned long long>(b.pairs),
                    static_cast<unsigned long long>(a.placements),
                    static_cast<unsigned long long>(b.placements),
                    static_cast<unsigned long long>(a.families),
                    static_cast<unsigned long long>(b.families),
                    static_cast<unsigned long long>(a.settlements),
                    static_cast<unsigned long long>(b.settlements));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_position_tail_retirement_work: %s\n", error.what());
        return 1;
    }
}
