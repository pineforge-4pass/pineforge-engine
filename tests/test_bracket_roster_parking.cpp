// R5 lane V19-D witness: a bracket family's roster parks the erased members
// behind a retained one, and stays the same roster.
//
// source::BracketRoster keeps every leg a strategy.exit family placed, in
// order (the bracket cancel cancels each of them, and a leg that no longer
// works leaves a NotWorking receipt whose ordinal is observable). V19-E kept
// the members whose placement rows were erased as a settled prefix of
// incarnations and every member from the first retained one onward as a
// handle, which the v4 fold reads in full: a family whose first leg a pin
// keeps (a leg of the current position cycle) held every later member as a
// handle, and a recording run paid a fold over all of them per bar (INT21's
// straddle: 0.15 / 1.67 / 23.5 s at 1k / 4k / 16k bars). V19-D parks every
// erased member behind a retained one in a run between the two handles it
// sits between.
//
// Randomized operation streams -- legs appended, rows erased (a member's row
// only ever goes), settles, and live legs removed (the bracket cancel of a
// part of the family) -- are played on a roster and on a plain model: the
// members in order, each marked erased or not. After every operation the
// roster must list the model's members in order (members(), iteration,
// size()); after every settle its working tail must be exactly the members
// whose rows are retained, count() and holds() must answer for them, and
// its fold words (settled_count(), settled_digest()) must be the model's:
// the maximal erased prefix's count and running digest, then each run of
// erased members between retained ones as (handles before it, count, digest).
// A second roster that settles only at the end reaches the same words.
//
// Fail-before, this TU against the lane's base 6c081f5d (spark aarch64,
// GCC 13, Release): it builds, and fails 282,041 of 2,237,468 checks, first
// roster.working_tail() == words.tail, roster.settled_digest() ==
// words.digest and roster.holds(member.handle) == held -- the working tail
// kept every member from the first retained one onward, erased ones
// included.
#include <pineforge/source/pine_adapter.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
using namespace pineforge;

int failures = 0;
int checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull | 1ull) {}
    std::uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
    bool percent(int p) { return below(100) < p; }
};

struct Member {
    native_order::RequestHandle handle;
    bool erased = false;
};

constexpr std::uint64_t kBasis = 1469598103934665603ULL;
std::uint64_t fold(std::uint64_t digest, std::uint64_t value) {
    return (digest ^ value) * 1099511628211ULL;
}

// The fold words the model says a settled roster shows.
struct Words {
    std::size_t count = 0;
    std::uint64_t digest = kBasis;
    std::vector<native_order::RequestHandle> tail;
};

Words expected(const std::vector<Member>& model) {
    Words words;
    std::size_t index = 0;
    for (; index < model.size() && model[index].erased; ++index) {
        ++words.count;
        words.digest = fold(words.digest, model[index].handle.incarnation);
    }
    // Runs of erased members between retained ones, each as the handles
    // before it, its count and its digest.
    std::size_t handles = 0;
    std::size_t run_count = 0;
    std::uint64_t run_digest = kBasis;
    std::vector<std::uint64_t> runs;
    const auto close_run = [&] {
        if (run_count == 0) return;
        runs.push_back(handles);
        runs.push_back(run_count);
        runs.push_back(run_digest);
        run_count = 0;
        run_digest = kBasis;
    };
    for (; index < model.size(); ++index) {
        if (model[index].erased) {
            ++run_count;
            run_digest = fold(run_digest, model[index].handle.incarnation);
            continue;
        }
        close_run();
        words.tail.push_back(model[index].handle);
        ++handles;
    }
    close_run();
    for (const auto word : runs) words.digest = fold(words.digest, word);
    return words;
}

bool same_members(const source::BracketRoster& roster, const std::vector<Member>& model) {
    const auto members = roster.members();
    if (members.size() != model.size() || roster.size() != model.size()) return false;
    std::size_t index = 0;
    for (const auto& handle : roster) {
        if (index >= model.size() || !(handle == model[index].handle)) return false;
        ++index;
    }
    for (std::size_t at = 0; at < model.size(); ++at)
        if (!(members[at] == model[at].handle)) return false;
    return index == model.size();
}

int streams = 0;
long operations = 0;
long parked_reads = 0;

void play(std::uint64_t seed) {
    Rng rng(seed);
    native_order::RunIdentity run;
    run.run_number = 7;
    source::BracketRoster roster;
    source::BracketRoster lazy;  // settles once, at the end
    std::vector<Member> model;
    std::uint64_t next = 1;
    const int steps = 200 + rng.below(400);
    const auto erased = [&](std::uint64_t incarnation) {
        for (const auto& member : model)
            if (member.handle.incarnation == incarnation) return member.erased;
        return false;
    };
    for (int step = 0; step < steps; ++step) {
        ++operations;
        const int op = rng.below(100);
        if (op < 40) {
            // A new leg (a fresh incarnation; now and then a gap).
            next += 1 + static_cast<std::uint64_t>(rng.percent(20) ? rng.below(40) : 0);
            const native_order::RequestHandle handle{run, next};
            roster.push_back(handle);
            lazy.push_back(handle);
            model.push_back({handle, false});
        } else if (op < 75) {
            // Rows go: mostly recent ones, the first retained one rarely.
            for (auto& member : model) {
                if (member.erased) continue;
                const bool first = &member == &*std::find_if(model.begin(), model.end(),
                    [](const Member& m) { return !m.erased; });
                if (rng.percent(first ? 3 : 35)) member.erased = true;
            }
        } else if (op < 92) {
            roster.settle(erased);
            const Words words = expected(model);
            CHECK(roster.working_tail() == words.tail);
            CHECK(roster.settled_count() == words.count);
            CHECK(roster.settled_digest() == words.digest);
            for (const auto& member : model) {
                const bool held = !member.erased;
                CHECK(roster.holds(member.handle) == held);
                if (held) CHECK(roster.count(member.handle.incarnation) == 1);
            }
            const std::size_t retained = static_cast<std::size_t>(std::count_if(
                model.begin(), model.end(), [](const Member& m) { return !m.erased; }));
            if (model.size() - words.count > retained) ++parked_reads;
        } else {
            // A cancel removes live legs: some of the retained members.
            std::vector<native_order::RequestHandle> doomed;
            for (const auto& member : model)
                if (!member.erased && rng.percent(30)) doomed.push_back(member.handle);
            if (doomed.empty()) continue;
            // remove_all's contract: a doomed member is in the working tail,
            // which a settle brings the lazy roster to as well.
            roster.settle(erased);
            lazy.settle(erased);
            roster.remove_all(doomed);
            lazy.remove_all(doomed);
            model.erase(std::remove_if(model.begin(), model.end(), [&](const Member& m) {
                return std::find(doomed.begin(), doomed.end(), m.handle) != doomed.end();
            }), model.end());
        }
        CHECK(same_members(roster, model));
        CHECK(same_members(lazy, model));
    }
    roster.settle(erased);
    lazy.settle(erased);
    const Words words = expected(model);
    CHECK(roster.working_tail() == words.tail);
    CHECK(lazy.working_tail() == words.tail);
    CHECK(roster.settled_count() == lazy.settled_count());
    CHECK(roster.settled_digest() == lazy.settled_digest());
    CHECK(lazy.settled_digest() == words.digest);
    CHECK(roster.empty() == model.empty());
    ++streams;
}

} // namespace

int main() {
    for (std::uint64_t seed = 1; seed <= 400; ++seed) play(seed);
    // Not vacuous: settles found erased members behind a retained one.
    CHECK(parked_reads > 1000);
    std::printf("test_bracket_roster_parking: %d streams, %ld operations, %ld settles with "
                "members parked; %d checks, %d failures\n",
                streams, operations, parked_reads, checks, failures);
    return failures == 0 ? 0 : 1;
}
