#pragma once

#include "history_drawing_store_fixture.hpp"
#include <pineforge/drawing.hpp>
#include <pineforge/series.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace history_drawing_test {

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Borrowed handles address a fixed, acyclic native fixture tree. Encoding
// a parent synchronizes its child containers before publishing the link.
struct Child {
    std::int32_t node = -1;
    std::int32_t sequence = 0;
};
struct ChildRecord {
    Child child;
    bool alive = false;
};
struct ChildCodec {
    using Wire = ChildRecord;
    static Wire encode(const ChildRecord&, pineforge::detail::o1::Store&);
    static ChildRecord decode(const Wire& wire, pineforge::detail::o1::Store&) { return wire; }
    static void release(const Wire&, pineforge::detail::o1::Store&) {}
};

} // namespace history_drawing_test

namespace pineforge::detail {
template<> struct DrawingStoreFor<history_drawing_test::ChildRecord> {
    using type = DrawingExternalRecords<history_drawing_test::ChildRecord,
                                        history_drawing_test::ChildCodec>;
};
} // namespace pineforge::detail

namespace history_drawing_test {

inline bool same(const Child& a, const Child& b) {
    return a.node == b.node && a.sequence == b.sequence;
}
inline bool same(const pineforge::LineRec& a, const pineforge::LineRec& b) {
    return a.x1 == b.x1 && a.x2 == b.x2 && a.y1 == b.y1 && a.y2 == b.y2 &&
        a.xloc == b.xloc && a.ext_l == b.ext_l && a.ext_r == b.ext_r && a.alive == b.alive;
}
inline bool same(const pineforge::BoxRec& a, const pineforge::BoxRec& b) {
    return a.left == b.left && a.right == b.right && a.top == b.top && a.bottom == b.bottom &&
        a.xloc == b.xloc && a.alive == b.alive;
}
inline bool same(const pineforge::LabelRec& a, const pineforge::LabelRec& b) {
    return a.x == b.x && a.y == b.y && a.xloc == b.xloc && a.yloc == b.yloc &&
        a.text == b.text && a.alive == b.alive;
}
inline bool same(const pineforge::LinefillRec& a, const pineforge::LinefillRec& b) {
    return a.line1 == b.line1 && a.line2 == b.line2 && a.alive == b.alive;
}

template<class Rec> Rec input(int i) {
    using namespace pineforge;
    if constexpr (std::is_same_v<Rec, LineRec>)
        return {i, i + 1, i * 0.25, i * 0.5, XLoc::bar_time, true, false, true};
    else if constexpr (std::is_same_v<Rec, BoxRec>)
        return {i, i + 2, i * 0.5, i * 0.25, XLoc::bar_index, true};
    else if constexpr (std::is_same_v<Rec, LabelRec>)
        return {i, i * 0.25, XLoc::bar_time, YLoc::abovebar,
                std::string(80, static_cast<char>('a' + i % 23)), true};
    else
        return {i, i + 1, true};
}
template<class Rec> void edit(Rec& rec) {
    using namespace pineforge;
    if constexpr (std::is_same_v<Rec, LineRec>) rec.y2 = -71.25;
    else if constexpr (std::is_same_v<Rec, BoxRec>) rec.top = -71.25;
    else if constexpr (std::is_same_v<Rec, LabelRec>) rec.text = std::string("child\0text", 10);
    else rec.line2 = -71;
}

template<class Rec> struct NativeArena {
    int cap;
    std::vector<Rec> records;
    std::deque<std::int32_t> order;
    explicit NativeArena(int capacity) : cap(capacity) {}
    std::int32_t alloc(Rec rec) {
        if (order.size() == static_cast<std::size_t>(cap)) {
            records[static_cast<std::size_t>(order.front())].alive = false;
            order.pop_front();
        }
        rec.alive = true;
        const auto id = static_cast<std::int32_t>(records.size());
        records.push_back(std::move(rec));
        order.push_back(id);
        return id;
    }
    void erase(std::int32_t id) {
        if (!records[static_cast<std::size_t>(id)].alive) return;
        records[static_cast<std::size_t>(id)].alive = false;
        order.erase(std::find(order.begin(), order.end(), id));
    }
    void check(const pineforge::DrawingArena<Rec>& value) const {
        require(value.size() == static_cast<std::int32_t>(records.size()),
                "DRAWING_HISTORY_DEPTH_STATE_RED: arena size");
        require(value.order() == order, "DRAWING_HISTORY_DEPTH_STATE_RED: arena FIFO order");
        for (std::size_t i = 0; i < records.size(); ++i) {
            const auto id = static_cast<std::int32_t>(i);
            require(same(value.at(id), records[i]) && value.alive(id) == records[i].alive,
                    "DRAWING_HISTORY_DEPTH_STATE_RED: arena record");
        }
    }
};

template<class Rec>
void arena_operations(pineforge::DrawingArena<Rec>& value, NativeArena<Rec>& native,
                      const std::shared_ptr<pineforge::detail::DrawingStorage>& storage) {
    for (int i = 0; i <= native.cap; ++i)
        require(value.alloc(input<Rec>(i)) == native.alloc(input<Rec>(i)),
                "DRAWING_HISTORY_DEPTH_STATE_RED: allocated id");
    // The first id was FIFO-evicted; deleted records retain writable fields.
    edit(value.at(0));
    edit(native.records[0]);
    const auto victim = native.order.front();
    value.erase(victim);
    native.erase(victim);
    value.erase(victim);
    const auto last = native.order.back();
    value.erase(last);
    native.erase(last);
    native.check(value);
    const auto checkpoint = value;
    const auto expected = native;
    edit(value.at(1));
    (void)value.alloc(input<Rec>(native.cap + 1));
    value = checkpoint;
    expected.check(value);
    // An arena has no clear() API: replacement by an empty value clears it.
    value = pineforge::DrawingArena<Rec>(native.cap, storage);
    require(value.size() == 0 && value.order().empty(),
            "DRAWING_HISTORY_DEPTH_STATE_RED: arena clear");
    value = checkpoint;
    native = expected;
    native.check(value);
    value.at(0) = native.records[0]; // leave a real dirty block for the nested flush
}

struct Node {
    static constexpr std::size_t kCapacity = 1031;
    pineforge::DynamicRingBuffer<Child> history;
    pineforge::Series<Child> series;
    pineforge::DrawingArena<pineforge::LineRec> lines;
    pineforge::DrawingArena<pineforge::BoxRec> boxes;
    pineforge::DrawingArena<pineforge::LabelRec> labels;
    pineforge::DrawingArena<pineforge::LinefillRec> fills;
    pineforge::DrawingArena<ChildRecord> children;
    std::vector<Child> expected_history;
    std::vector<Child> expected_series;
    NativeArena<pineforge::LineRec> expected_lines{1025};
    NativeArena<pineforge::BoxRec> expected_boxes{3};
    NativeArena<pineforge::LabelRec> expected_labels{3};
    NativeArena<pineforge::LinefillRec> expected_fills{3};
    std::int32_t child;
    explicit Node(const std::shared_ptr<pineforge::detail::DrawingStorage>& storage,
                  std::int32_t next)
        : history(kCapacity, storage), series(static_cast<int>(kCapacity), storage),
          lines(1025, storage), boxes(3, storage), labels(3, storage), fills(3, storage),
          children(3, storage), child(next) {}

    void prepare(const std::shared_ptr<pineforge::detail::DrawingStorage>& storage) {
        for (int i = 0; i < 4; ++i) {
            const Child ref{child, i};
            history.push_front(ref);
            expected_history.insert(expected_history.begin(), ref);
            series.push(ref);
            expected_series.insert(expected_series.begin(), ref);
        }
        history.update_front({child, 17});
        expected_history.front() = {child, 17};
        series.update({child, 19});
        expected_series.front() = {child, 19};
        const auto saved_history = history;
        const auto saved_series = series;
        history.resize(2);
        history.push_front({child, 29});
        require(history.size() == 2 && same(history[1], expected_history[0]),
                "DRAWING_HISTORY_DEPTH_STATE_RED: history drop/evict");
        history = saved_history;
        series.clear();
        history.clear();
        require(history.size() == 0 && series.size() == 0,
                "DRAWING_HISTORY_DEPTH_STATE_RED: history clear");
        history = saved_history;
        series = saved_series;
        arena_operations(lines, expected_lines, storage);
        arena_operations(boxes, expected_boxes, storage);
        arena_operations(labels, expected_labels, storage);
        arena_operations(fills, expected_fills, storage);
        require(children.alloc({{child, 37}, true}) == 0,
                "DRAWING_HISTORY_DEPTH_STATE_RED: child link id");
        check();
        // Restore copied roots then dirty the handle histories, so every
        // level synchronizes real child state while its parent is encoding.
        history.update_front(expected_history.front());
        series.update(expected_series.front());
    }
    void check() const {
        require(history.size() == expected_history.size() && history.capacity() == kCapacity &&
                series.size() == static_cast<int>(expected_series.size()),
                "DRAWING_HISTORY_DEPTH_STATE_RED: history shape");
        for (std::size_t i = 0; i < expected_history.size(); ++i)
            require(same(history[i], expected_history[i]),
                    "DRAWING_HISTORY_DEPTH_STATE_RED: history value");
        for (std::size_t i = 0; i < expected_series.size(); ++i)
            require(same(series[static_cast<int>(i)], expected_series[i]),
                    "DRAWING_HISTORY_DEPTH_STATE_RED: series value");
        require(same(series.current(), expected_series.front()),
                "DRAWING_HISTORY_DEPTH_STATE_RED: series current");
        expected_lines.check(lines);
        expected_boxes.check(boxes);
        expected_labels.check(labels);
        expected_fills.check(fills);
        const auto link = children.at(0);
        require(children.size() == 1 && link.alive && link.child.node == child &&
                link.child.sequence == 37, "DRAWING_HISTORY_DEPTH_STATE_RED: child link value");
    }
    void flush() const;
};

inline std::array<Node*, 6> nodes{};
inline std::size_t current_depth = 0;
inline std::size_t deepest = 0;
inline std::array<std::size_t, 6> visits{};

template<class T>
auto flush_storage(const T& value, int) -> decltype(value.storage_stats(), void()) {
    (void)value.storage_stats();
}
template<class T> void flush_storage(const T&, long) {}

inline void Node::flush() const {
    struct Visit {
        Visit() { deepest = std::max(deepest, ++current_depth); }
        ~Visit() { --current_depth; }
    } visit;
    const auto index = static_cast<std::size_t>(child < 0 ? 5 : child - 1);
    ++visits[index];
    (void)history.storage_stats();
    (void)series.storage_stats();
    flush_storage(lines, 0);
    flush_storage(boxes, 0);
    flush_storage(labels, 0);
    flush_storage(fills, 0);
    (void)children.storage_stats();
}

inline ChildCodec::Wire ChildCodec::encode(const ChildRecord& rec,
                                           pineforge::detail::o1::Store&) {
    if (rec.child.node >= 0) {
        const auto index = static_cast<std::size_t>(rec.child.node);
        require(index < nodes.size() && nodes[index],
                "DRAWING_HISTORY_DEPTH_STATE_RED: invalid child handle");
        nodes[index]->flush();
    }
    return rec;
}

inline void depth_six() {
    StoreFixture fixture;
    auto& handles = fixture.handles;
    const pineforge::detail::o1::Store& real = *fixture.real;
    require(handles.live == 0 && handles.peak == 0 && real.stats().max_live_handles == 0,
            "DRAWING_HISTORY_DEPTH_HANDLES_RED: dirty counter");
    current_depth = deepest = 0;
    visits = {};
    // Every explicit begin goes through caller_begin. The expected handle
    // counts below are this count, never a number read off a run.
    std::size_t explicit_begins = 0;
    const auto caller_begin = [&] {
        auto handle = fixture.storage->store->begin();
        ++explicit_begins;
        return handle;
    };
    const auto caller_handles_only = [&] {
        return real.stats().max_live_handles == explicit_begins &&
               handles.peak == explicit_begins && handles.live == explicit_begins &&
               handles.begins == explicit_begins;
    };
    {
        auto outer = caller_begin();
        require(caller_handles_only(),
                "DRAWING_HISTORY_DEPTH_HANDLES_RED: outer handle was not counted");
        {
            auto joined = caller_begin();
            require(caller_handles_only(),
                    "DRAWING_HISTORY_DEPTH_HANDLES_RED: joined handle was not counted");
            const auto joins = handles.joined;
            {
                std::array<std::optional<Node>, 6> tree;
                for (std::size_t i = tree.size(); i-- > 0;) {
                    const auto child = i + 1 < tree.size() ? static_cast<std::int32_t>(i + 1) : -1;
                    tree[i].emplace(fixture.storage, child);
                    nodes[i] = &*tree[i];
                    tree[i]->prepare(fixture.storage);
                }
                tree[0]->flush();
                for (const auto& node : tree) node->check();
                require(deepest == tree.size() && current_depth == 0 &&
                        std::all_of(visits.begin(), visits.end(), [](std::size_t n) { return n != 0; }),
                        "DRAWING_HISTORY_DEPTH_HANDLES_RED: the tree did not reach six levels");
                // The outer and joined hooks are the only handles. Every
                // container operation, the public one and each child codec's,
                // joins the open transaction through Store::scope() and takes
                // no handle: depth contributes zero.
                require(caller_handles_only(),
                        "DRAWING_HISTORY_DEPTH_HANDLES_RED: nesting retained extra live handles");
                // Each level's link flush encodes the next level inside the
                // previous one: at least one joined operation per level.
                require(handles.joined >= joins + tree.size() && handles.abandoned == 0,
                        "DRAWING_HISTORY_DEPTH_HANDLES_RED: the container operations did not join "
                        "the caller transaction");
            }
            nodes = {};
            joined->commit();
            require(handles.live == explicit_begins,
                    "DRAWING_HISTORY_DEPTH_HANDLES_RED: commit destroyed a live handle");
        }
        require(handles.live == explicit_begins - 1,
                "DRAWING_HISTORY_DEPTH_HANDLES_RED: joined handle leaked");
        outer->commit();
    }
    require(handles.live == 0 && handles.peak == explicit_begins &&
            handles.begins == explicit_begins && handles.abandoned == 0 &&
            real.stats().max_live_handles == explicit_begins,
            "DRAWING_HISTORY_DEPTH_HANDLES_RED: handle lifetime count did not return to zero");
    std::printf("history_drawing_depth_six: PASS depth=%zu explicit_begins=%zu "
                "max_live_handles=%llu live=%zu joined=%zu\n",
                deepest, explicit_begins,
                static_cast<unsigned long long>(real.stats().max_live_handles), handles.live,
                handles.joined);
}

} // namespace history_drawing_test
