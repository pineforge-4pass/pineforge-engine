#pragma once

// This entire fixture uses the frozen2 public drawing/host APIs. Compile it
// with frozen2's drawing.hpp first: the SAME byte bound must fail numerically,
// after both the callback population and deleted-record checks have run.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/drawing.hpp>
#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace a5_drawing_fixture {
using namespace pineforge;

inline void require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

// Design arithmetic, NEVER a sampled ceiling. Four record stores, each with
// 64 x 4096 cache bytes, one page for its 64 frame descriptors (24 bytes each
// on the lab ABI), and two pages for scalar owners/control blocks, the fixed
// Tx handle and file-open scratch. Two typed 32-record working blocks per
// version plus one decode block can own label text at once. There are at most
// two retained versions here. The 96-byte input's string allocation is 97
// bytes, including its terminator. Four cap-sized rosters cover live, saved
// and the two assignment temporaries. OHLCV and host allocations are outside
// the drawing-only recording windows; drawing preparation is INCLUDED.
inline constexpr std::size_t kPage = 4096;
inline constexpr std::size_t kPages = 64;
inline constexpr std::size_t kWidth = 32;
inline constexpr std::size_t kText = 96;
inline constexpr std::size_t kCaps = 17 + 11 + 13 + 7;
inline constexpr std::size_t kBound =
    4 * (kPages + 3) * kPage + (2 * 2 + 1) * kWidth * (kText + 1) +
    4 * kCaps * sizeof(std::int32_t) + 2 * (kText + 1);
// A large declared cap uses four further Store/cache owners for sparse FIFO
// slots. Its two int blocks and scalar owners fit the same three extra pages.
inline constexpr std::size_t kExternalOrderBound =
    kBound + 4 * (kPages + 3) * kPage;

inline std::size_t peak = 0;
inline void record_peak(std::size_t) {
    peak = std::max(peak, global_allocation::live_bytes);
}

struct DrawingWindow {
    bool previous = global_allocation::recording;
    DrawingWindow() { global_allocation::recording = true; }
    ~DrawingWindow() { global_allocation::recording = previous; }
};

struct State {
    DrawingArena<LineRec> lines;
    DrawingArena<BoxRec> boxes;
    DrawingArena<LabelRec> labels;
    DrawingArena<LinefillRec> fills;

    explicit State(bool external_order = false)
        : lines(external_order ? std::numeric_limits<int>::max() : 17),
          boxes(external_order ? std::numeric_limits<int>::max() : 11),
          labels(external_order ? std::numeric_limits<int>::max() : 13),
          fills(external_order ? std::numeric_limits<int>::max() : 7) {}
};

inline double close_at(std::size_t bar) {
    return 100.0 + static_cast<double>((bar * 37U) % 4096U) * 0.25;
}

// Script state is real drawing arenas, driven once per accepted native
// calculation through the public kernel. No substitute/model runs this path.
class ChurnHost final : public NativeStrategyHost {
    State& state_;
    std::size_t callbacks_ = 0;
    bool external_order_;
public:
    ChurnHost(State& state, bool external_order)
        : state_(state), external_order_(external_order) {
        NativeRunSpec spec;
        spec.identity = {"a5-drawing-growth", 1};
        spec.input_tf = "1";
        spec.script_tf = "1";
        spec.ticker = "DRAW";
        spec.tickerid = "TEST:DRAW";
        spec.type = "crypto";
        spec.currency = "USDT";
        spec.basecurrency = "ETH";
        spec.timezone = "UTC";
        spec.session = "24x7";
        spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
        spec.initial_capital = 1000000.0;
        spec.point_value = 1.0;
        spec.account_fx = 1.0;
        spec.price_tick = 0.25;
        spec.event_retention = NativeEventRetention::Window;
        require(configure_native(spec).status == NativeSetupStatus::Applied,
                "configure drawing growth host");
    }
    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        DrawingWindow window;
        const auto i = static_cast<std::int32_t>(callbacks_);
        require(bar.close == close_at(callbacks_), "unexpected synthetic bar");
        const Line line = pf_line_new(state_.lines, i, i + 0.25, i + 1, i + 0.75);
        const Box box = pf_box_new(state_.boxes, i, i + 1.25, i + 1, i - 0.25);
        const Label label = pf_label_new(state_.labels, i, i + 0.5,
            std::string(kText, static_cast<char>('a' + i % 26)));
        const Linefill fill = pf_linefill_new(state_.fills, line, Line{i - 1});
        require(line.id == i && box.id == i && label.id == i && fill.id == i,
                "drawing identity was recycled or a record was dropped");
        // Keep a deleted record in the large-cap case as well.
        if (i == 0 || (!external_order_ && i % 5 == 0)) {
            pf_line_delete(state_.lines, line);
            pf_box_delete(state_.boxes, box);
            pf_label_delete(state_.labels, label);
            pf_linefill_delete(state_.fills, fill);
        }
        ++callbacks_;
    }
    std::size_t callbacks() const { return callbacks_; }
};

template<class Arena, class = void> struct HasStats : std::false_type {};
template<class Arena> struct HasStats<Arena,
    std::void_t<decltype(std::declval<const Arena&>().storage_stats())>> : std::true_type {};

template<class Arena>
void stats(const Arena& arena, const char* kind, std::size_t count) {
    if constexpr (HasStats<Arena>::value) {
        const auto value = arena.storage_stats();
        std::printf("A5 DRAW store=%s bars=%zu resident=%llu backing=%llu\n", kind, count,
                    static_cast<unsigned long long>(value.bytes_resident),
                    static_cast<unsigned long long>(value.bytes_backing));
        require(value.bytes_resident <= kPages * kPage, "record Store cache grew");
        require(value.bytes_backing > kPages * kPage, "test never crossed Store cache");
    }
}

inline void check_deleted(State& live, State& saved, std::size_t count) {
    require(!live.lines.alive(0) && !live.boxes.alive(0) &&
            !live.labels.alive(0) && !live.fills.alive(0), "old tombstone revived");
    require(pf_line_get_x1(live.lines, Line{0}) == 0, "deleted line lost geometry");
    require(pf_line_get_x2(live.lines, Line{0}) == 1 &&
            pf_line_get_y1(live.lines, Line{0}) == 0.25 &&
            pf_line_get_y2(live.lines, Line{0}) == 0.75, "deleted line lost nonzero geometry");
    require(pf_box_get_top(live.boxes, Box{0}) == 1.25, "deleted box lost geometry");
    require(pf_label_get_text(live.labels, Label{0}) == std::string(kText, 'a'),
            "deleted label lost text");
    require(pf_linefill_get_line1(live.fills, Linefill{0}).id == 0,
            "deleted linefill lost geometry");
    require(pf_linefill_get_line2(live.fills, Linefill{0}).id == -1,
            "deleted linefill lost its second reference");
    pf_line_set_x1(live.lines, Line{0}, -7);
    pf_box_set_top(live.boxes, Box{0}, -8.0);
    pf_label_set_text(live.labels, Label{0}, std::string("late\0text", 9));
    live.fills.at(0).line1 = -9;
    // Force every dirty working block out with distant reads before checking.
    const auto last = static_cast<std::int32_t>(count - 1);
    const auto middle = static_cast<std::int32_t>(count / 2);
    require(pf_line_get_x1(live.lines, Line{middle}) == middle, "middle line changed");
    require(pf_box_get_top(live.boxes, Box{middle}) == middle + 1.25, "middle box changed");
    require(pf_label_get_x(live.labels, Label{middle}) == middle, "middle label changed");
    require(pf_linefill_get_line1(live.fills, Linefill{middle}).id == middle, "middle fill changed");
    require(pf_line_get_x1(live.lines, Line{last}) == last, "new line changed");
    require(pf_box_get_top(live.boxes, Box{last}) == last + 1.25, "new box changed");
    require(pf_label_get_x(live.labels, Label{last}) == last, "new label changed");
    require(pf_linefill_get_line1(live.fills, Linefill{last}).id == last, "new fill changed");
    require(pf_line_get_x1(live.lines, Line{0}) == -7, "deleted line mutation lost");
    require(pf_line_get_y1(live.lines, Line{0}) == 0.25, "deleted line edit damaged other fields");
    require(pf_box_get_top(live.boxes, Box{0}) == -8.0, "deleted box mutation lost");
    require(pf_label_get_text(live.labels, Label{0}) == std::string("late\0text", 9),
            "deleted label mutation lost");
    require(pf_linefill_get_line1(live.fills, Linefill{0}).id == -9,
            "deleted fill mutation lost");
    require(pf_line_get_x1(saved.lines, Line{0}) == 0, "snapshot line was changed");
    require(pf_box_get_top(saved.boxes, Box{0}) == 1.25, "snapshot box was changed");
    require(pf_label_get_text(saved.labels, Label{0}) == std::string(kText, 'a'),
            "snapshot label was changed");
    require(pf_linefill_get_line1(saved.fills, Linefill{0}).id == 0, "snapshot fill was changed");
}

// A new, previously uncopied State for EACH population: no warmed copy path
// can hide the first-snapshot cost. Recording includes construction and every
// operation that may free a previously recorded arena allocation.
inline int growth(std::size_t count, bool external_order) {
    require(global_allocation::live_bytes == 0, "drawing measurement leaked a prior population");
    require(count * sizeof(LineRec) > kExternalOrderBound,
            "drawing population cannot exceed even the larger fixed bound");
    std::vector<Bar> bars(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double close = close_at(i);
        bars[i] = {close, close + 1.0, close - 1.0, close, 1000.0,
                   INT64_C(1736121600000) + static_cast<std::int64_t>(i) * 60000};
    }
    peak = 0;
    std::optional<State> live, saved;
    {
        DrawingWindow window;
        live.emplace(external_order);
    }
    ChurnHost host(*live, external_order);
    host.run(bars.data(), static_cast<int>(count), "1", "1");
    require(host.last_error().empty(), host.last_error().c_str());
    require(host.callbacks() == count, "drawing host did not reach the claimed bar count");
    const auto execution_peak = peak;
    int failures = 0;
    {
        DrawingWindow window;
        require(live->lines.size() == static_cast<std::int32_t>(count) &&
                live->boxes.size() == static_cast<std::int32_t>(count) &&
                live->labels.size() == static_cast<std::int32_t>(count) &&
                live->fills.size() == static_cast<std::int32_t>(count), "drawing slot count changed");
        const auto retained = global_allocation::live_bytes;
        peak = retained;
        saved.emplace(*live); // The FIRST snapshot, including its transient allocations.
        const auto snapshot_peak = peak;
        pf_line_set_x1(live->lines, Line{0}, -123);
        pf_label_set_text(live->labels, Label{static_cast<std::int32_t>(count - 1)}, "speculative");
        peak = global_allocation::live_bytes;
        *live = *saved;
        const auto restore_peak = peak;
        check_deleted(*live, *saved, count);
        *live = *saved;
        if (external_order) {
            require(live->lines.order().size() == count - 1, "sparse FIFO truncated live records");
            require(live->lines.order().front() == 1 &&
                    live->lines.order().back() == static_cast<std::int32_t>(count - 1),
                    "sparse FIFO endpoints changed");
        }
        stats(live->lines, "line", count);
        stats(live->boxes, "box", count);
        stats(live->labels, "label", count);
        stats(live->fills, "linefill", count);
        const auto bound = external_order ? kExternalOrderBound : kBound;
        std::printf("A5 DRAW bars=%zu external_order=%d retained=%zu execution_peak=%zu "
                    "first_snapshot_peak=%zu restore_peak=%zu bound=%zu\n",
                    count, external_order, retained, execution_peak, snapshot_peak, restore_peak, bound);
        if (retained > bound) ++failures;
        if (execution_peak > bound) ++failures;
        if (snapshot_peak > bound) ++failures;
        if (restore_peak > bound) ++failures;
        // This diagnostic distinguishes a real numerical RED from setup or
        // semantic failures in the frozen-header lab control.
        if (failures) std::printf("A5 DRAW RESIDENT_BOUND_RED failures=%d\n", failures);
        saved.reset();
        live.reset();
    }
    require(global_allocation::live_bytes == 0, "drawing owners leaked retained heap");
    return failures;
}

// Exercise the actual roster type returned by the public arena. The baseline
// type is a deque (empty constructor); the new type takes its declared cap.
// References/iterator categories are deliberately not assumed to be random access.
template<class Ring>
Ring prepared_ring(std::size_t cap) {
    if constexpr (std::is_same_v<Ring, std::deque<std::int32_t>>) return Ring{};
    else return Ring(cap);
}
inline int roster_allocations() {
    using Ring = std::decay_t<decltype(std::declval<DrawingArena<LineRec>&>().order())>;
    int failures = 0;
    for (std::size_t cap : {1U, 2U, 17U, 500U}) {
        auto ring = prepared_ring<Ring>(cap);
        auto saved = prepared_ring<Ring>(cap);
        std::deque<std::int32_t> oracle;
        std::size_t allocated = 0;
        for (std::int32_t id = 0; id < 10000; ++id) {
            if (oracle.size() == cap) oracle.pop_front();
            oracle.push_back(id);
            const auto before = global_allocation::allocations;
            if (ring.size() == cap) ring.pop_front();
            ring.push_back(id);
            allocated += global_allocation::allocations - before;
            require(ring.size() == oracle.size() &&
                    std::equal(ring.begin(), ring.end(), oracle.begin()), "FIFO wrap changed order");
            if (id % 7 == 0) {
                const auto position = static_cast<std::size_t>(id) % ring.size();
                auto actual = ring.begin();
                std::advance(actual, static_cast<std::ptrdiff_t>(position));
                oracle.erase(oracle.begin() + static_cast<std::ptrdiff_t>(position));
                const auto erase_before = global_allocation::allocations;
                ring.erase(actual);
                allocated += global_allocation::allocations - erase_before;
            }
            if (id % 31 == 0) {
                const auto copy_before = global_allocation::allocations;
                saved = ring;
                ring.clear();
                ring = saved;
                allocated += global_allocation::allocations - copy_before;
                require(ring.size() == oracle.size() &&
                        std::equal(ring.begin(), ring.end(), oracle.begin()), "FIFO restore changed order");
            }
        }
        std::printf("A5 DRAW roster cap=%zu allocation_delta=%zu\n", cap, allocated);
        if (allocated) ++failures;
    }
    return failures;
}

inline int run(std::size_t count) {
    try {
        global_allocation::on_recorded = record_peak;
        int failures = growth(count, false);
        failures += growth(count, true);
        failures += roster_allocations();
        // Independent growing-heap control crosses the SAME derived ceiling.
        {
            DrawingWindow window;
            std::vector<LineRec> control(kExternalOrderBound / sizeof(LineRec) + 1);
            control.back().x1 = 42;
            require(control.back().x1 == 42 && global_allocation::live_bytes > kExternalOrderBound,
                    "heap control did not reach the fixed drawing ceiling");
        }
        global_allocation::on_recorded = nullptr;
        std::printf("A5 DRAW bars=%zu failures=%d\n", count, failures);
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        global_allocation::on_recorded = nullptr;
        std::fprintf(stderr, "A5 DRAW semantic/setup failure: %s\n", error.what());
        return 2;
    }
}
} // namespace a5_drawing_fixture
