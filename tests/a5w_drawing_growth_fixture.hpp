#pragma once

// Drawing records, live order and checkpoint copies stay resident-O(1) in bars.
//
// FAIL-BEFORE. The same source compiles against main's drawing.hpp
// (f66318a3) and against the external record store: it uses the public arena
// API only, and reads the store's statistics only where an arena offers
// storage_stats(). On main every record ever created stays on the heap (a
// vector, then shared heap blocks once copied; the first copy copies them
// all) and the live order is a std::deque, so the expected first failures
// there are RESIDENT_BOUND_RED and ROSTER_ALLOCATION_RED (exit 1), printed
// after every exact read below has passed. A semantic or setup failure exits
// 2, never 1, so it cannot pass for a RED.
//
// One script shape, lowered as codegen lowers it: one line, box, label and
// linefill per bar through the pf_* calls, three field writes, every fifth
// bar a delete of all four, through a real NativeStrategyHost over synthetic
// deterministic OHLCV. Only drawing calls run inside a recording window, so
// caller-owned OHLCV and host state are outside the measurement. The run with
// finite caps (17/11/13/7, exact FIFO eviction) takes its first snapshot after
// the last bar; the run with caps of INT_MAX (no eviction, the live order
// grows with bars) also takes a calc_on_order_fills cycle every 997 bars:
// snapshot, a speculative execution, restore. Both then take a snapshot, a
// speculative execution with late writes to deleted records and a restore,
// and read back every field of every record, every liveness flag and the
// whole *.all order against generators and an independent FIFO model that
// live only in this file.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/drawing.hpp>
#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace drawing_growth_test {

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// ---- the bound, derived from the design (never from a measurement) --------
// One opened drawing family (a record kind's store, or a live order above one
// page of ids): the ScratchStore workspace for eight resident 4 KiB pages --
// eight cache and six work pages, 8 x (24-byte slot + two 4-byte buckets) =
// 256 bytes of metadata in one page, and two pages for its Core and alignment
// (tests/test_o1_store.cpp:184 ceiling_for, which that row checks against
// bytes_resident) -- plus the object layer's 16-byte header and 1024-byte
// chunk buffer, plus one page for the fixed owners: DrawingStorage with its
// shared_ptr control block, the file backing, StoreImpl, CommandScratchStore
// and one Tx handle at a time.
inline constexpr std::size_t kPage = 4096;
inline constexpr std::size_t kFamilyBound = (8 + 6 + 1 + 2) * kPage + 16 + 1024 + kPage;
// Label text: 96 bytes, embedded NULs included. Its heap request is 97 bytes
// under libstdc++ (capacity + 1) and 112 under libc++ (rounded to 16): 128
// covers both. Decoded texts: two 32-record working blocks in each of the two
// arena values this fixture holds (live and saved) and one 32-record decode
// temporary; plus two call-boundary strings (an argument moved in, a copy read
// out).
inline constexpr std::size_t kText = 96;
inline constexpr std::size_t kTextHeap = 128;
static_assert(kTextHeap >= (kText + 1 + 15) / 16 * 16, "one label text's heap request");
inline constexpr std::size_t kLabelTexts = (2 * 2 + 1) * 32 * kTextHeap + 2 * kTextHeap;
// The finite caps' live rings, one per kind in each of the two values.
inline constexpr std::size_t kRingIds = 2 * (17 + 11 + 13 + 7) * sizeof(std::int32_t);
// Opening a family builds the scratch file's path from the directory name and
// frees it before open returns: the directory, the path with its 25-byte file
// suffix, a selection note and a create message, each under name + 512 bytes
// of fixed text and at most doubled by string growth. Sequential opens free
// their strings before the next one.
inline std::size_t open_transient() {
    const char* configured = std::getenv("TMPDIR");
    const std::size_t name = configured != nullptr ? std::strlen(configured) : 0;
    return 4 * 2 * (name + 512);
}
inline std::size_t drawing_bound(std::size_t families) {
    return families * kFamilyBound + open_transient() + kLabelTexts + kRingIds;
}

// ---- measurement -----------------------------------------------------------
inline std::size_t peak = 0;
inline void record_peak(std::size_t) {
    peak = std::max(peak, global_allocation::live_bytes);
}
struct Window {
    bool previous = global_allocation::recording;
    Window() { global_allocation::recording = true; }
    ~Window() { global_allocation::recording = previous; }
};
// The test-only oracle is never part of the family's measurement.
struct Unrecorded {
    bool previous = global_allocation::recording;
    Unrecorded() { global_allocation::recording = false; }
    ~Unrecorded() { global_allocation::recording = previous; }
};

inline double value_at(std::size_t bar) {
    return 100.0 + static_cast<double>(bar) * 2.0 +
           static_cast<double>((bar * 37) % 8) * 0.25;
}
inline std::string text_at(std::size_t bar) {
    std::string result(kText, '\0');
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<char>((bar * 13 + i * 29) % 251);
    return result;
}

template<class Step>
class Host final : public pineforge::NativeStrategyHost {
    Step& step_;
    std::size_t callbacks_ = 0;
public:
    explicit Host(Step& step) : step_(step) {
        pineforge::NativeRunSpec spec;
        spec.identity = {"drawing-growth", 1};
        spec.input_tf = "1";
        spec.script_tf = "1";
        spec.ticker = "DRAWINGS";
        spec.tickerid = "TEST:DRAWINGS";
        spec.type = "crypto";
        spec.currency = "USDT";
        spec.basecurrency = "ETH";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.slot_label_policy = pineforge::NativeSlotLabelPolicy::FeedTolerant;
        spec.initial_capital = 1000000.0;
        spec.point_value = 1.0;
        spec.account_fx = 1.0;
        spec.price_tick = 0.25;
        spec.event_retention = pineforge::NativeEventRetention::Window;
        require(configure_native(spec).status == pineforge::NativeSetupStatus::Applied,
                "configure the drawing growth host");
    }
    void on_native_bar(const pineforge::Bar& bar,
                       const pineforge::NativeDecisionContext&) override {
        require(bar.close == value_at(callbacks_), "unexpected synthetic input");
        Window window;
        step_(callbacks_++);
    }
    std::size_t callbacks() const { return callbacks_; }
};

template<class Step>
void drive(std::size_t count, Step& step) {
    std::vector<pineforge::Bar> bars(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double close = value_at(i);
        bars[i] = {close, close + 1.0, close - 1.0, close, 1000.0,
                   INT64_C(1736121600000) + static_cast<std::int64_t>(i) * 60000};
    }
    Host<Step> host(step);
    host.run(bars.data(), static_cast<int>(count), "1", "1");
    require(host.last_error().empty(), host.last_error().c_str());
    require(host.callbacks() == count, "the host did not reach its claimed population");
}

template<class T, class = void> struct HasStats : std::false_type {};
template<class T> struct HasStats<T, std::void_t<
    decltype(std::declval<const T&>().storage_stats())>> : std::true_type {};

// GREEN only: the population really left the resident window and none of it
// was dropped. main's arenas have no store and skip this.
template<class Arena>
void check_spilled(const Arena& arena, std::size_t payload, const char* kind) {
    if constexpr (HasStats<Arena>::value) {
        const auto stats = arena.storage_stats();
        require(stats.bytes_live >= payload, "a record was dropped from the external store");
        require(stats.bytes_backing > stats.bytes_resident,
                "the population never left the resident window");
        std::printf("drawings %s live=%llu resident=%llu backing=%llu\n", kind,
                    static_cast<unsigned long long>(stats.bytes_live),
                    static_cast<unsigned long long>(stats.bytes_resident),
                    static_cast<unsigned long long>(stats.bytes_backing));
    }
}

struct Drawings {
    pineforge::DrawingArena<pineforge::LineRec> lines;
    pineforge::DrawingArena<pineforge::BoxRec> boxes;
    pineforge::DrawingArena<pineforge::LabelRec> labels;
    pineforge::DrawingArena<pineforge::LinefillRec> fills;
    static int cap(bool sparse, int finite) {
        return sparse ? std::numeric_limits<int>::max() : finite;
    }
    explicit Drawings(bool sparse)
        : lines(cap(sparse, 17)), boxes(cap(sparse, 11)), labels(cap(sparse, 13)),
          fills(cap(sparse, 7)) {}
};

// The *.all order and the liveness of every id, rebuilt from the script's own
// operations: eviction of the oldest at cap, then the newest deleted every
// fifth bar.
template<class Arena>
void check_order(const Arena& arena, std::size_t count, std::size_t cap) {
    std::vector<std::int32_t> order;
    std::vector<bool> alive;
    {
        Unrecorded unrecorded;
        alive.assign(count, false);
        for (std::size_t bar = 0; bar < count; ++bar) {
            if (order.size() == cap) {
                alive[static_cast<std::size_t>(order.front())] = false;
                order.erase(order.begin());
            }
            order.push_back(static_cast<std::int32_t>(bar));
            alive[bar] = true;
            if (bar % 5 == 0) {
                order.pop_back();
                alive[bar] = false;
            }
        }
    }
    require(order.size() == arena.order().size() &&
            std::equal(order.begin(), order.end(), arena.order().begin()),
            "the *.all order changed");
    for (std::size_t i = 0; i < count; ++i)
        require(arena.alive(static_cast<std::int32_t>(i)) == alive[i], "a liveness flag changed");
    require(!arena.alive(-1) && !arena.alive(static_cast<std::int32_t>(count)),
            "an id outside the population is alive");
}

// Returns true when a phase's retained or peak heap exceeded the bound.
inline bool drawing_growth(std::size_t count, bool sparse) {
    using namespace pineforge;
    const std::size_t families = sparse ? 8 : 4;
    const std::size_t bound = drawing_bound(families);
    require(count * sizeof(LineRec) > bound, "the line population cannot cross the bound");
    require(global_allocation::live_bytes == 0, "the drawing measurement starts dirty");
    peak = 0;
    std::optional<Drawings> live, saved;
    {
        Window window;
        live.emplace(sparse);
    }
    std::size_t cycles = 0;
    auto step = [&](std::size_t bar) {
        const auto id = static_cast<std::int32_t>(bar);
        const double value = value_at(bar);
        const Line line = pf_line_new(live->lines, id, value, id + 1, value + 0.5,
                                      XLoc::bar_index, bar % 2 != 0, bar % 3 != 0);
        const Box box = pf_box_new(live->boxes, id, value + 1.0, id + 2, value - 1.0);
        const Label label = pf_label_new(live->labels, id, value + 0.25, text_at(bar),
                                         XLoc::bar_time, static_cast<YLoc>(bar % 3));
        const Linefill fill = pf_linefill_new(live->fills, line, Line{id - 1});
        require(line.id == id && box.id == id && label.id == id && fill.id == id,
                "a drawing id was recycled, refused or not rolled back");
        pf_line_set_x2(live->lines, line, id + 3);
        pf_box_set_left(live->boxes, box, id - 2);
        pf_label_set_x(live->labels, label, id + 5);
        if (bar % 5 == 0) {
            pf_line_delete(live->lines, line);
            pf_box_delete(live->boxes, box);
            pf_label_delete(live->labels, label);
            pf_linefill_delete(live->fills, fill);
        }
        if (sparse && bar % 997 == 996) {
            // Snapshot, a speculative execution (a new field, an old spilled
            // label, a delete, a new id), restore: none of it may survive.
            saved.emplace(*live);
            pf_line_set_y2(live->lines, line, -value);
            pf_label_set_text(live->labels, Label{id / 2}, text_at(bar + 1));
            pf_box_delete(live->boxes, box);
            pf_line_new(live->lines, -1, -1.0, -2, -2.0);
            *live = *saved;
            ++cycles;
        }
    };
    drive(count, step);
    require(!sparse || cycles == count / 997, "a checkpoint cycle was skipped");
    const std::size_t execution_peak = peak;
    std::size_t snapshot_peak = 0, restore_peak = 0, late_peak = 0;
    {
        Window window;
        peak = global_allocation::live_bytes;
        saved.emplace(*live);
        snapshot_peak = peak;
        // A speculative execution: late writes to the oldest records, all of
        // them deleted at bar 0, and the next id.
        pf_line_set_y1(live->lines, Line{0}, -value_at(count));
        pf_box_set_top(live->boxes, Box{0}, -value_at(count + 1));
        pf_label_set_text(live->labels, Label{0}, text_at(count + 2));
        live->fills.at(0).line2 = static_cast<std::int32_t>(count);
        const Line extra = pf_line_new(live->lines, -1, -1.0, -2, -2.0);
        require(extra.id == static_cast<std::int32_t>(count), "the next id is not the next one");
        require(!live->lines.alive(0) && pf_line_get_y1(live->lines, Line{0}) == -value_at(count) &&
                pf_box_get_top(live->boxes, Box{0}) == -value_at(count + 1) &&
                pf_label_get_text(live->labels, Label{0}) == text_at(count + 2) &&
                pf_linefill_get_line2(live->fills, Linefill{0}).id == static_cast<std::int32_t>(count),
                "a late write to a deleted record was lost");
        require(pf_line_get_y1(saved->lines, Line{0}) == value_at(0) &&
                pf_box_get_top(saved->boxes, Box{0}) == value_at(0) + 1.0 &&
                pf_label_get_text(saved->labels, Label{0}) == text_at(0) &&
                pf_linefill_get_line2(saved->fills, Linefill{0}).id == -1 &&
                saved->lines.size() == static_cast<std::int32_t>(count),
                "the snapshot saw a later write");
        peak = global_allocation::live_bytes;
        *live = *saved;
        restore_peak = peak;
        require(live->lines.size() == static_cast<std::int32_t>(count) &&
                live->boxes.size() == static_cast<std::int32_t>(count) &&
                live->labels.size() == static_cast<std::int32_t>(count) &&
                live->fills.size() == static_cast<std::int32_t>(count),
                "the restore kept a speculative id");
        // Every field of every record, live and dead, after the restore.
        for (std::size_t bar = 0; bar < count; ++bar) {
            const auto id = static_cast<std::int32_t>(bar);
            const double value = value_at(bar);
            const LineRec line = std::as_const(live->lines).at(id);
            const BoxRec box = std::as_const(live->boxes).at(id);
            const LabelRec label = std::as_const(live->labels).at(id);
            const LinefillRec fill = std::as_const(live->fills).at(id);
            require(line.x1 == id && line.x2 == id + 3 && line.y1 == value &&
                    line.y2 == value + 0.5 && line.xloc == XLoc::bar_index &&
                    line.ext_l == (bar % 2 != 0) && line.ext_r == (bar % 3 != 0),
                    "a line field changed");
            require(box.left == id - 2 && box.right == id + 2 && box.top == value + 1.0 &&
                    box.bottom == value - 1.0 && box.xloc == XLoc::bar_index,
                    "a box field changed");
            require(label.x == id + 5 && label.y == value + 0.25 &&
                    label.xloc == XLoc::bar_time && label.yloc == static_cast<YLoc>(bar % 3) &&
                    label.text == text_at(bar), "a label field changed");
            require(fill.line1 == id && fill.line2 == id - 1, "a linefill field changed");
        }
        const std::size_t open = std::numeric_limits<std::size_t>::max();
        check_order(live->lines, count, sparse ? open : 17);
        check_order(live->boxes, count, sparse ? open : 11);
        check_order(live->labels, count, sparse ? open : 13);
        check_order(live->fills, count, sparse ? open : 7);
        check_spilled(live->lines, count * sizeof(LineRec), "line");
        check_spilled(live->boxes, count * sizeof(BoxRec), "box");
        check_spilled(live->labels, count * kText, "label");
        check_spilled(live->fills, count * sizeof(LinefillRec), "linefill");
        late_peak = peak;
        saved.reset();
        live.reset();
    }
    require(global_allocation::live_bytes == 0, "the drawing owners leaked heap");
    const bool red = execution_peak > bound || snapshot_peak > bound ||
                     restore_peak > bound || late_peak > bound;
    std::printf("drawing_growth bars=%zu caps=%s execution=%zu first_snapshot=%zu restore=%zu "
                "late_read=%zu bound=%zu %s\n",
                count, sparse ? "int_max" : "finite", execution_peak, snapshot_peak,
                restore_peak, late_peak, bound, red ? "RESIDENT_BOUND_RED" : "PASS");
    return red;
}

// The live order allocates nothing after the arena is constructed. A
// record kind outside the four keeps the heap vector store, whose growth is
// geometric (factor >= 1.5): at most ceil(log_1.5(count)) + 1 reallocations.
// Every further allocation is the order's own; main's std::deque allocates a
// block per 128 ids (libstdc++) or 1024 (libc++) pushed.
struct RosterRec {
    std::int64_t x = 0;
    bool alive = false;
};

inline bool roster_allocations(std::size_t count) {
    std::size_t growth = 1;
    for (double capacity = 1.0; capacity < static_cast<double>(count); capacity *= 1.5) ++growth;
    pineforge::DrawingArena<RosterRec> arena(7);
    const std::size_t before = global_allocation::allocations;
    for (std::size_t i = 0; i < count; ++i) {
        RosterRec rec;
        rec.x = static_cast<std::int64_t>(i);
        require(arena.alloc(rec) == static_cast<std::int32_t>(i), "a roster id was recycled");
        if (i % 3 == 0) arena.erase(static_cast<std::int32_t>(i));
    }
    const std::size_t allocations = global_allocation::allocations - before;
    // The live ids: the newest seven not deleted, oldest first.
    std::vector<std::int32_t> want;
    for (std::size_t i = 0; i < count; ++i) {
        if (want.size() == 7) want.erase(want.begin());
        want.push_back(static_cast<std::int32_t>(i));
        if (i % 3 == 0) want.pop_back();
    }
    require(want.size() == arena.order().size() &&
            std::equal(want.begin(), want.end(), arena.order().begin()),
            "the roster order changed");
    require(arena.at(0).x == 0 && !arena.alive(0), "a deleted roster record changed");
    const bool red = allocations > growth;
    std::printf("drawing_roster bars=%zu allocations=%zu bound=%zu %s\n", count, allocations, growth,
                red ? "ROSTER_ALLOCATION_RED" : "PASS");
    return red;
}

inline int run(std::size_t count) {
    try {
        global_allocation::on_recorded = record_peak;
        const bool finite = drawing_growth(count, false);
        const bool sparse = drawing_growth(count, true);
        global_allocation::on_recorded = nullptr;
        const bool roster = roster_allocations(count);
        return finite || sparse || roster ? 1 : 0;
    } catch (const std::exception& error) {
        global_allocation::on_recorded = nullptr;
        std::fprintf(stderr, "drawings semantic/setup failure: %s\n", error.what());
        return 2;
    }
}

} // namespace drawing_growth_test
