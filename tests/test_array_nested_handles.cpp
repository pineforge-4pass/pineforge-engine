#include <pineforge/array.hpp>
#include <pineforge/drawing.hpp>
#include <pineforge/engine.hpp>
#include "array_store_fixture.hpp"

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace {
using namespace pineforge;
using array_test::require;

void nested(detail::o1::Store& store) {
    using Child = ScriptArray<double>;
    using Parent = ScriptArray<Child>;
    const auto before = store.stats().bytes_live;
    {
        auto child = Child::from(store, {1.0, 2.0, 3.0});
        Parent parent(store);
        parent.push(child);
        child.set_i(0, 99.0);
        require(parent.get_i(0).first() == 1.0, "nested write stored a borrowed child");
        auto read_copy = parent.get_i(0);
        read_copy.set_i(0, 77.0);
        require(parent.get_i(0).first() == 1.0, "nested read aliases parent storage");
        parent.mutate(0, [](Child& value) { value.push(4.0); value.set_i(0, 10.0); });
        require(parent.get_i(0).size() == 4 && parent.get_i(0).first() == 10.0,
                "nested lvalue mutation did not write back");
        parent.unshift(child);
        parent.insert(1, child);
        require(parent.remove_i(1).first() == 99.0, "nested remove returned wrong value");
        require(parent.last().first() == 10.0, "nested shift damaged another child");
        auto copy = parent.copy();
        parent.clear();
        require(copy.size() == 2 && copy.last().last() == 4.0, "nested copy lost a released subtree");
        ScriptArray<Parent> graph(store);
        graph.push(copy);
        graph.unshift(copy);
        graph.remove_i(0);
        require(graph.first().last().first() == 10.0, "three-level ownership move changed values");
        const auto retained = store.stats().bytes_live;
        for (int i = 0; i < 128; ++i) {
            auto transient = graph.first();
            transient.clear();
        }
        require(store.stats().bytes_live == retained, "nested reads leak descendant objects");
        BrokerStateHashSink first, second;
        graph.hash_into(first);
        auto equivalent = graph.copy();
        equivalent.hash_into(second);
        require(first.h == second.h, "nested hash depends on allocation ids");
        equivalent.mutate(0, [](Parent& p) {
            p.mutate(0, [](Child& c) { c.set_i(0, -5.0); });
        });
        BrokerStateHashSink changed;
        equivalent.hash_into(changed);
        require(first.h != changed.h, "nested durable value is absent from the hash");
    }
    require(store.stats().bytes_live == before, "recursive release leaked descendants");
}

void deleted_handles(detail::o1::Store& store) {
    static_assert(sizeof(Line) == 4 && sizeof(Box) == 4 &&
                  sizeof(Label) == 4 && sizeof(Linefill) == 4, "drawing handle widths");
    static_assert(ScriptArray<Line>::element_bytes == 4, "line array stored arena records");
    static_assert(ScriptArray<Box>::element_bytes == 4, "box array stored arena records");
    static_assert(ScriptArray<Label>::element_bytes == 4, "label array stored arena records");
    static_assert(ScriptArray<Linefill>::element_bytes == 4, "linefill array stored arena records");
    DrawingArena<LineRec> lines(2);
    DrawingArena<BoxRec> boxes(2);
    DrawingArena<LabelRec> labels(2);
    DrawingArena<LinefillRec> fills(2);
    const auto line = pf_line_new(lines, 7, 11.0, 9, 13.0);
    const auto box = pf_box_new(boxes, 4, 20.0, 8, 10.0);
    const auto label = pf_label_new(labels, 6, 12.0, "retained after delete");
    const auto fill = pf_linefill_new(fills, line, line);
    ScriptArray<Line> line_ids(store);
    ScriptArray<Box> box_ids(store);
    ScriptArray<Label> label_ids(store);
    ScriptArray<Linefill> fill_ids(store);
    line_ids.push(line); box_ids.push(box); label_ids.push(label); fill_ids.push(fill);
    pf_line_delete(lines, line); pf_box_delete(boxes, box);
    pf_label_delete(labels, label); pf_linefill_delete(fills, fill);
    require(!lines.alive(line.id) && !boxes.alive(box.id) &&
            !labels.alive(label.id) && !fills.alive(fill.id), "records were not deleted");
    // Every parent passes the 8-page store window with 4-byte cells, while
    // arena growth is constant: append the SAME deleted handle on each bar.
    constexpr std::int64_t count = 100000;
    for (std::int64_t i = 1; i < count; ++i) {
        line_ids.push(line); box_ids.push(box); label_ids.push(label); fill_ids.push(fill);
    }
    require(line_ids.size() == count && fill_ids.size() == count, "drawing id array dropped cells");
    require(line_ids.get_i(0).id == line.id && box_ids.get_i(0).id == box.id &&
            label_ids.get_i(0).id == label.id && fill_ids.get_i(0).id == fill.id,
            "late read dropped a deleted handle");
    require(pf_line_get_x1(lines, line_ids.get_i(0)) == 7, "deleted line geometry lost");
    pf_line_set_x2(lines, line_ids.get_i(0), 1234);
    require(pf_line_get_x2(lines, line_ids.get_i(0)) == 1234, "deleted line no longer writable");
    require(pf_box_get_top(boxes, box_ids.get_i(0)) == 20.0, "deleted box geometry lost");
    require(pf_label_get_text(labels, label_ids.get_i(0)) == "retained after delete",
            "deleted label text lost");
    require(pf_linefill_get_line1(fills, fill_ids.get_i(0)).id == line.id, "deleted fill lost line id");
    ScriptArray<ScriptArray<Line>> nested_lines(store);
    nested_lines.push(line_ids);
    require(pf_line_get_x1(lines, nested_lines.first().first()) == 7, "nested deleted handle changed");
}
} // namespace

int main() {
    try {
        array_test::StoreFixture fixture;
        const auto live = fixture.store->stats().bytes_live;
        nested(*fixture.store);
        deleted_handles(*fixture.store);
        require(fixture.store->stats().bytes_live == live, "array handle storage leaked");
        std::puts("array nested arrays and deleted drawing records: exact late reads");
        return 0;
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "array nested/handles: %s\n", failure.what());
        return 1;
    }
}
