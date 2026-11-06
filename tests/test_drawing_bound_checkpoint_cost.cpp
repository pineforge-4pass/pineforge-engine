// K-DRAWBOUND: a bounded drawing arena's edits and checkpoints cost O(changes), not O(bound).
//
// A bounded arena (DrawingArena built from a DrawingBoundConfig: what the generated
// code constructs from a script's declared max_lines_count, max_boxes_count and
// max_labels_count) keeps its occupants in one state object that an arena and its
// copies share. Every create, every delete and every setter used to replace that
// state with a deep copy of the whole of it -- every slot, the text of every label,
// the directory and the order -- whether or not a checkpoint still shared it. A
// script that moves a few of its live drawings on every bar therefore paid O(bound)
// per call, and the first write after each checkpoint paid it once more. The unbound
// arena was already O(changes): its records live in blocks of 32 that copies share,
// and a write clones only the blocks it reaches (DrawingRecordStore, K-DRAWSNAP; the
// unbound twin of this test is test_coof_drawing_checkpoint_cost).
//
// The shape is a script with a bounded set of live levels (the strategy that exposed
// it keeps up to five hundred lines and labels alive and, on every bar, sets two
// fields of each active level): an arena full to its bound, and per script execution,
// under the scheduler's checkpoint sequence (snapshot at the bar open, restore,
// execute, commit; the generated hooks hold every arena by value),
//
//   label.delete(oldest)
//   label.new(...)                         a text of 200 characters, never inline
//   label.set_x(oldest live), label.set_x(a middle one), label.set_y(the new one)
//   label.set_text(second oldest)          another text of 200 characters
//
// Every record copy therefore allocates the text it carries, so what a run allocates
// counts the records it copied.
//
// Rows:
//   cost       per execution, after the checkpoint is restored: how many of the arena's
//              records are not the very objects the checkpoint holds (a record the
//              arena shares with its checkpoint is the same object through both), and
//              how many heap blocks the restore and the body allocate, at bound 100 and
//              at bound 1,000. One execution writes five slots (the one it deletes, the
//              one it creates in, the oldest live label, the middle one and the one
//              whose text changes), each in one block of 32 records that the checkpoint
//              shares, so it may copy five blocks, and the test allows six (the tail
//              block a store keeps beside its tree): 192 records, and 6 * (32 + 1)
//              blocks of heap plus 80 for the state, the directory, the order and the
//              texts it builds, whatever the bound. Before K-DRAWBOUND every call copied
//              every slot: 1,000 records unshared and some 6,000 blocks at bound 1,000.
//   model      after every execution the arena equals a plain model of the same
//              operations (size, order, and every field of every live record), so the
//              cheaper copy changes no value.
//   unchanged  checkpointing an arena nobody wrote allocates the same at bound 1,000 as
//              at bound 100: sharing is O(1) in the bound.
//
// Under AddressSanitizer (whose fake stack makes every call dear) the cost row runs a
// third of its executions.
#include "global_allocation_replacement.hpp"

#include <pineforge/drawing.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <utility>

using namespace pineforge;

#if defined(__SANITIZE_ADDRESS__)
constexpr int kSanitized = 1;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr int kSanitized = 1;
#else
constexpr int kSanitized = 0;
#endif
#else
constexpr int kSanitized = 0;
#endif
constexpr int kExecutionDivisor = kSanitized ? 3 : 1;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++g_fail;                                                          \
        } else {                                                               \
            ++g_pass;                                                          \
        }                                                                      \
    } while (0)

namespace {

constexpr int kBlock = 32;           // records per block a store shares between copies
constexpr int kTouchedBlocks = 6;    // five slots written per execution, one block of slack
constexpr int kTextLength = 200;     // never inline: every copy of a record allocates
constexpr int kWarmup = 3;           // executions before the measured ones (the first copy of a fresh state)
constexpr int kUnchangedBars = 1000 / kExecutionDivisor;

std::string text_of(char first, int n) {
    return std::string(static_cast<std::size_t>(kTextLength), static_cast<char>(first + n % 26));
}

// ---- the model: the same operations on a plain ordered map -------------------
struct Model {
    std::map<std::int32_t, LabelRec> live;   // by logical id, which is also creation order
    std::int32_t next = 0;
};

void model_create(Model& m, std::int64_t x, double y, std::string text) {
    m.live.emplace(m.next, LabelRec{x, y, XLoc::bar_index, YLoc::price, std::move(text), true});
    ++m.next;
}

// Every observable of the arena against the model.
bool same(const DrawingArena<LabelRec>& arena, const Model& m) {
    if (arena.size() != m.next) return false;
    if (arena.occupied_slots() != m.live.size()) return false;
    const auto& order = arena.order();
    if (order.size() != m.live.size()) return false;
    std::size_t i = 0;
    for (const auto& entry : m.live) {
        const std::int32_t id = entry.first;
        const LabelRec& want = entry.second;
        if (order[i++] != id) return false;
        if (arena.access_status(id) != DrawingAccessStatus::present) return false;
        const LabelRec& got = arena.at(id);
        if (got.x != want.x || got.y != want.y || got.xloc != want.xloc || got.yloc != want.yloc
            || got.text != want.text || !got.alive)
            return false;
    }
    return true;
}

// ---- one script execution, on the arena and on its model ---------------------
void body(DrawingArena<LabelRec>& arena, int bar) {
    pf_label_delete(arena, Label{arena.order().front()});
    pf_label_new(arena, bar, 100.0 + bar, text_of('a', bar));
    const std::int32_t first = arena.order().front();
    const std::int32_t next = arena.size();
    pf_label_set_x(arena, Label{first}, bar);
    pf_label_set_x(arena, Label{first + (next - first) / 2}, bar);
    pf_label_set_y(arena, Label{next - 1}, 1.0 * bar);
    pf_label_set_text(arena, Label{first + 1}, text_of('A', bar));
}

void model_body(Model& m, int bar) {
    m.live.erase(m.live.begin());
    model_create(m, bar, 100.0 + bar, text_of('a', bar));
    const std::int32_t first = m.live.begin()->first;
    const std::int32_t next = m.next;
    m.live.at(first).x = bar;
    m.live.at(first + (next - first) / 2).x = bar;
    m.live.at(next - 1).y = 1.0 * bar;
    m.live.at(first + 1).text = text_of('A', bar);
}

// How many of the arena's records are not the records its checkpoint holds: a
// record the two share is the same object through both, a record one write copied
// (with its block) is another, and a record the checkpoint never had is new.
std::uint64_t unshared(const DrawingArena<LabelRec>& live, const DrawingArena<LabelRec>& before) {
    std::uint64_t n = 0;
    for (const std::int32_t id : live.order()) {
        const bool shared = before.access_status(id) == DrawingAccessStatus::present
            && &live.at(id) == &before.at(id);
        if (!shared) ++n;
    }
    return n;
}

struct Cost {
    int executions = 0;
    std::uint64_t unshared_total = 0;
    std::uint64_t unshared_max = 0;
    std::uint64_t allocations_total = 0;
    std::uint64_t allocations_max = 0;
    bool model_agrees = true;
    bool final_state_ok = false;
};

Cost run(int bound, int executions) {
    DrawingArena<LabelRec> arena(drawing_bound(bound));
    Model model;
    for (int i = 0; i < bound; ++i) {
        pf_label_new(arena, i, 1.0 * i, text_of('a', i));
        model_create(model, i, 1.0 * i, text_of('a', i));
    }
    std::optional<DrawingArena<LabelRec>> checkpoint;
    Cost cost;
    for (int bar = 0; bar < executions + kWarmup; ++bar) {
        checkpoint.emplace(arena);                           // snapshot: the bar open
        const std::size_t before = global_allocation::allocations;
        arena = *checkpoint;                                 // restore
        body(arena, bar);                                    // execute
        const std::size_t after = global_allocation::allocations;
        model_body(model, bar);
        if (!same(arena, model)) cost.model_agrees = false;
        if (bar >= kWarmup) {
            const std::uint64_t allocated = after - before;
            const std::uint64_t records = unshared(arena, *checkpoint);
            ++cost.executions;
            cost.unshared_total += records;
            cost.unshared_max = std::max(cost.unshared_max, records);
            cost.allocations_total += allocated;
            cost.allocations_max = std::max(cost.allocations_max, allocated);
        }
        checkpoint.emplace(arena);                           // commit
    }
    // The run ends as the model does: the bound's worth of live labels, the oldest
    // `executions + warm-up` ids deleted, and one id issued per creation.
    const int total = executions + kWarmup;
    cost.final_state_ok = arena.occupied_slots() == static_cast<std::size_t>(bound)
        && arena.size() == bound + total
        && arena.order().front() == total
        && arena.order().back() == bound + total - 1
        && arena.last_allocation_diagnostic().status == DrawingAllocationStatus::ok;
    return cost;
}

void test_edits_and_checkpoints_cost_o_changes() {
    std::printf("test_edits_and_checkpoints_cost_o_changes\n");
    const int executions = 150 / kExecutionDivisor;
    const int bounds[] = {100, 1000};
    for (const int bound : bounds) {
        const Cost c = run(bound, executions);
        const double mean_unshared = c.executions
            ? static_cast<double>(c.unshared_total) / c.executions : 0.0;
        const double mean_allocations = c.executions
            ? static_cast<double>(c.allocations_total) / c.executions : 0.0;
        std::printf("  bound %4d: %d executions; records not shared with the checkpoint per "
                    "execution: mean %.1f, max %llu (limit %d); heap blocks per execution: mean "
                    "%.1f, max %llu (limit %d)\n",
                    bound, c.executions, mean_unshared, (unsigned long long)c.unshared_max,
                    kTouchedBlocks * kBlock, mean_allocations,
                    (unsigned long long)c.allocations_max, kTouchedBlocks * (kBlock + 1) + 80);
        CHECK(c.executions == executions);
        CHECK(c.model_agrees);
        CHECK(c.final_state_ok);
        // The arena writes at most six blocks of 32 records per execution, whatever
        // the bound; a record any other write copied would be O(bound).
        CHECK(c.unshared_max <= static_cast<std::uint64_t>(kTouchedBlocks * kBlock));
        // And what it allocates for them: a copy of each block's records (their
        // texts), the block, and a fixed few for the state, the directory, the order
        // and the texts the body builds.
        CHECK(c.allocations_max
              <= static_cast<std::uint64_t>(kTouchedBlocks * (kBlock + 1) + 80));
    }
}

// ---- an unchanged checkpoint allocates nothing that grows with the bound -----
std::size_t unchanged_checkpoint_allocations(int bound) {
    DrawingArena<LabelRec> live(drawing_bound(bound));
    for (int i = 0; i < bound; ++i) pf_label_new(live, i, 1.0 * i, text_of('a', i));
    std::optional<DrawingArena<LabelRec>> checkpoint;
    checkpoint.emplace(live);   // the first bar: snapshot, restore, commit
    live = *checkpoint;
    checkpoint.emplace(live);
    const std::size_t before = global_allocation::allocations;
    for (int bar = 0; bar < kUnchangedBars; ++bar) {
        checkpoint.emplace(live);   // snapshot
        live = *checkpoint;         // restore
        checkpoint.emplace(live);   // commit
    }
    return global_allocation::allocations - before;
}

void test_unchanged_checkpoint_allocates_nothing_that_grows() {
    std::printf("test_unchanged_checkpoint_allocates_nothing_that_grows\n");
    const std::size_t small = unchanged_checkpoint_allocations(100);
    const std::size_t large = unchanged_checkpoint_allocations(1000);
    std::printf("  %d bars of snapshot / restore / commit allocate %zu blocks at bound 100 "
                "and %zu at bound 1,000\n", kUnchangedBars, small, large);
    CHECK(large == small);
}

}  // namespace

int main() {
    test_edits_and_checkpoints_cost_o_changes();
    test_unchanged_checkpoint_allocates_nothing_that_grows();
    std::printf("\nbounded drawing cost tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
