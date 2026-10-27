#pragma once

// Compile these same public-API workloads against main before the port.
// RED must be a RESIDENT_BOUND_RED after all callbacks and exact reads, never
// a missing symbol or a setup failure. Both closures run on the lab.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/drawing.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/series.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace history_growth_test {

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// ScratchStore: eight cache pages, six work pages, one metadata page
// (8 * (24-byte slot + two 4-byte buckets)), two Core/alignment pages;
// object layer: a 16-byte header and 1024-byte chunk buffer.
// Two more pages cover fixed storage/backing/shared owners, Tx and path
// construction. None of these terms depends on the workload population.
inline constexpr std::size_t kPage = 4096;
inline constexpr std::size_t kStoreBound = (8 + 6 + 1 + 2 + 2) * kPage + 16 + 1024;
inline constexpr std::size_t kText = 96;
inline constexpr std::size_t kDrawingBound =
    8 * kStoreBound + 5 * 32 * (kText + 1) +
    4 * (17 + 11 + 13 + 7) * sizeof(std::int32_t) + 2 * (kText + 1);
inline constexpr std::size_t kHistoryBound = kStoreBound + 2 * kPage;

inline std::size_t peak = 0;
inline void record_peak(std::size_t) {
    peak = std::max(peak, global_allocation::live_bytes);
}
struct Window {
    bool previous = global_allocation::recording;
    Window() { global_allocation::recording = true; }
    ~Window() { global_allocation::recording = previous; }
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
        spec.identity = {"history-growth", 1};
        spec.input_tf = "1";
        spec.script_tf = "1";
        spec.ticker = "HISTORY";
        spec.tickerid = "TEST:HISTORY";
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
                "configure history growth host");
    }
    void on_native_bar(const pineforge::Bar& bar,
                       const pineforge::NativeDecisionContext&) override {
        require(bar.close == value_at(callbacks_), "unexpected synthetic input");
        step_(callbacks_++, bar.close);
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
    require(host.callbacks() == count, "host did not reach its claimed population");
}

template<class T, class = void> struct HasStats : std::false_type {};
template<class T> struct HasStats<T, std::void_t<
    decltype(std::declval<const T&>().storage_stats())>> : std::true_type {};

template<class T>
void check_store(const T& value, std::size_t payload, const char* kind, std::size_t stores = 1) {
    if constexpr (HasStats<T>::value) {
        const auto stats = value.storage_stats();
        require(stats.bytes_resident <= stores * kStoreBound, "store workspace grew");
        require(stats.bytes_live >= payload, "logical population was dropped");
        require(stats.bytes_backing > stats.bytes_resident, "population never spilled");
        std::printf("history %s live=%llu resident=%llu backing=%llu\n", kind,
                    static_cast<unsigned long long>(stats.bytes_live),
                    static_cast<unsigned long long>(stats.bytes_resident),
                    static_cast<unsigned long long>(stats.bytes_backing));
    }
}

inline int history_growth(std::size_t count) {
    require(count * sizeof(double) > kHistoryBound, "history cannot cross bound");
    require(global_allocation::live_bytes == 0, "history measurement starts dirty");
    peak = 0;
    std::optional<pineforge::DynamicRingBuffer<double>> history, saved;
    {
        Window window;
        history.emplace(0);
    }
    auto step = [&](std::size_t bar, double value) {
        Window window;
        // This is the raw API's previously unbounded replacement-vector path.
        if (history->capacity() <= bar)
            history->resize(std::min(count, (bar + 1) * 2));
        history->push_front(value);
        for (const std::size_t offset : {std::size_t{0}, bar / 7, bar / 2, bar})
            require((*history)[offset] == value_at(bar - offset), "history read changed");
        require(pineforge::is_na((*history)[bar + 1]), "history read past window");
    };
    drive(count, step);
    const auto execution_peak = peak;
    std::size_t snapshot_peak = 0, restore_peak = 0, late_peak = 0;
    {
        Window window;
        require(history->size() == count && history->capacity() == count,
                "raw resize changed its declared window");
        peak = global_allocation::live_bytes;
        saved.emplace(*history);
        snapshot_peak = peak;
        history->update_front(-value_at(count));
        history->resize(count / 3);
        history->push_front(-value_at(count + 1));
        peak = global_allocation::live_bytes;
        *history = *saved;
        restore_peak = peak;
        // Late read of EVERY retained element after speculative update,
        // resize and push. The oracle is the input generator, not the ring.
        for (std::size_t offset = 0; offset < count; ++offset)
            require((*history)[offset] == value_at(count - offset - 1),
                    "history rollback changed an old value");
        check_store(*history, count * sizeof(double), "history");
        late_peak = peak;
        saved.reset();
        history.reset();
    }
    require(global_allocation::live_bytes == 0, "history owners leaked heap");
    const bool red = execution_peak > kHistoryBound ||
                     snapshot_peak > kHistoryBound || restore_peak > kHistoryBound ||
                     late_peak > kHistoryBound;
    std::printf("history_growth bars=%zu execution=%zu first_snapshot=%zu restore=%zu late_read=%zu bound=%zu %s\n",
                count, execution_peak, snapshot_peak, restore_peak, late_peak, kHistoryBound,
                red ? "RESIDENT_BOUND_RED" : "PASS");
    return red ? 1 : 0;
}

struct Drawings {
    pineforge::DrawingArena<pineforge::LineRec> lines;
    pineforge::DrawingArena<pineforge::BoxRec> boxes;
    pineforge::DrawingArena<pineforge::LabelRec> labels;
    pineforge::DrawingArena<pineforge::LinefillRec> fills;
    explicit Drawings(bool sparse)
        : lines(sparse ? std::numeric_limits<int>::max() : 17),
          boxes(sparse ? std::numeric_limits<int>::max() : 11),
          labels(sparse ? std::numeric_limits<int>::max() : 13),
          fills(sparse ? std::numeric_limits<int>::max() : 7) {}
};

inline int drawing_growth(std::size_t count, bool sparse) {
    using namespace pineforge;
    require(count * sizeof(LineRec) > kDrawingBound, "drawings cannot cross bound");
    require(global_allocation::live_bytes == 0, "drawing measurement starts dirty");
    peak = 0;
    std::optional<Drawings> live, saved;
    {
        Window window;
        live.emplace(sparse);
    }
    auto step = [&](std::size_t bar, double value) {
        Window window;
        const auto id = static_cast<std::int32_t>(bar);
        const auto line = pf_line_new(live->lines, id, value, id + 1, value + 0.5,
                                     XLoc::bar_index, bar % 2 != 0, bar % 3 != 0);
        const auto box = pf_box_new(live->boxes, id, value + 1.0, id + 2, value - 1.0);
        const auto label = pf_label_new(live->labels, id, value + 0.25, text_at(bar),
                                       XLoc::bar_time, static_cast<YLoc>(bar % 3));
        const auto fill = pf_linefill_new(live->fills, line, Line{id - 1});
        require(line.id == id && box.id == id && label.id == id && fill.id == id,
                "drawing id was recycled or refused");
        // Move fields, then delete. Deleted values must remain readable.
        pf_line_set_x2(live->lines, line, id + 3);
        pf_box_set_left(live->boxes, box, id - 2);
        pf_label_set_x(live->labels, label, id + 5);
        if (bar % 5 == 0) {
            pf_line_delete(live->lines, line);
            pf_box_delete(live->boxes, box);
            pf_label_delete(live->labels, label);
            pf_linefill_delete(live->fills, fill);
        }
    };
    drive(count, step);
    const auto execution_peak = peak;
    std::size_t snapshot_peak = 0, restore_peak = 0, late_peak = 0;
    {
        Window window;
        peak = global_allocation::live_bytes;
        saved.emplace(*live);
        snapshot_peak = peak;
        pf_line_set_y1(live->lines, Line{0}, -value_at(count));
        pf_box_set_top(live->boxes, Box{0}, -value_at(count + 1));
        pf_label_set_text(live->labels, Label{0}, text_at(count + 2));
        live->fills.at(0).line2 = static_cast<std::int32_t>(count);
        pf_line_new(live->lines, -1, -1.0, -2, -2.0);
        peak = global_allocation::live_bytes;
        *live = *saved;
        restore_peak = peak;
        require(live->lines.size() == static_cast<std::int32_t>(count) &&
                live->boxes.size() == static_cast<std::int32_t>(count) &&
                live->labels.size() == static_cast<std::int32_t>(count) &&
                live->fills.size() == static_cast<std::int32_t>(count), "drawing size rollback");
        // All record fields, including tombstones and embedded-NUL strings.
        for (std::size_t bar = 0; bar < count; ++bar) {
            const auto id = static_cast<std::int32_t>(bar);
            const auto value = value_at(bar);
            const auto line = std::as_const(live->lines).at(id);
            const auto box = std::as_const(live->boxes).at(id);
            const auto label = std::as_const(live->labels).at(id);
            const auto fill = std::as_const(live->fills).at(id);
            require(line.x1 == id && line.x2 == id + 3 &&
                    line.y1 == value && line.y2 == value + 0.5 &&
                    line.xloc == XLoc::bar_index && line.ext_l == (bar % 2 != 0) &&
                    line.ext_r == (bar % 3 != 0), "line field rollback");
            require(box.left == id - 2 && box.right == id + 2 &&
                    box.top == value + 1.0 && box.bottom == value - 1.0 &&
                    box.xloc == XLoc::bar_index, "box field rollback");
            require(label.x == id + 5 && label.y == value + 0.25 &&
                    label.xloc == XLoc::bar_time &&
                    label.yloc == static_cast<YLoc>(bar % 3) &&
                    label.text == text_at(bar), "label field rollback");
            require(fill.line1 == id && fill.line2 == id - 1, "linefill field rollback");
            if (sparse) {
                const bool alive = bar % 5 != 0;
                require(line.alive == alive && box.alive == alive &&
                        label.alive == alive && fill.alive == alive, "sparse tombstone rollback");
            }
        }
        // Independent FIFO model reproduces eviction BEFORE explicit delete.
        const auto check_order = [&](const auto& arena, std::size_t cap) {
            global_allocation::recording = false;
            std::vector<std::int32_t> order;
            std::vector<bool> alive(count, false);
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
            global_allocation::recording = true;
            require(order.size() == arena.order().size() &&
                    std::equal(order.begin(), order.end(), arena.order().begin()),
                    "drawing FIFO rollback");
            for (std::size_t i = 0; i < count; ++i)
                require(arena.alive(static_cast<std::int32_t>(i)) == alive[i],
                        "drawing alive flag rollback");
        };
        // The test-only oracle allocations are excluded from the family peak.
        check_order(live->lines, sparse ? count : 17);
        check_order(live->boxes, sparse ? count : 11);
        check_order(live->labels, sparse ? count : 13);
        check_order(live->fills, sparse ? count : 7);
        check_store(live->lines, count * sizeof(LineRec), "line", sparse ? 2 : 1);
        check_store(live->boxes, count * sizeof(BoxRec), "box", sparse ? 2 : 1);
        check_store(live->labels, count * kText, "label", sparse ? 2 : 1);
        check_store(live->fills, count * sizeof(LinefillRec), "linefill", sparse ? 2 : 1);
        late_peak = peak;
        saved.reset();
        live.reset();
    }
    require(global_allocation::live_bytes == 0, "drawing owners leaked heap");
    const bool red = execution_peak > kDrawingBound ||
                     snapshot_peak > kDrawingBound || restore_peak > kDrawingBound ||
                     late_peak > kDrawingBound;
    std::printf("drawing_growth bars=%zu sparse=%d execution=%zu first_snapshot=%zu restore=%zu late_read=%zu bound=%zu %s\n",
                count, sparse, execution_peak, snapshot_peak, restore_peak, late_peak, kDrawingBound,
                red ? "RESIDENT_BOUND_RED" : "PASS");
    return red ? 1 : 0;
}

inline int run(std::size_t count, bool history) {
    try {
        global_allocation::on_recorded = record_peak;
        const int failures = history ? history_growth(count) :
            drawing_growth(count, false) + drawing_growth(count, true);
        global_allocation::on_recorded = nullptr;
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        global_allocation::on_recorded = nullptr;
        std::fprintf(stderr, "history semantic/setup failure: %s\n", error.what());
        return 2;
    }
}

} // namespace history_growth_test
