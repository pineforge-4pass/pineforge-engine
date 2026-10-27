// Store admission limits met through drawing arenas and a history ring: the
// caller handle pool and the per-Store live-object limit.
//
// Pool row. The caller holds every pool handle with explicit begin() calls.
// Drawing and history operations inside still run, because each joins the
// open transaction through Store::scope(). One more begin() throws
// TxPoolExhausted, takes no handle and changes neither the arenas, the
// history nor the caller's transaction, which keeps working and commits every
// joined change; the returned slot is taken again.
//
// Limit row. At a small configured live-object limit, line records reach the
// Store exactly up to the limit (CAP). The next object (CAP+1) throws
// ScratchCapacityError with the cohort argument from a lone allocation, from
// one joined to a caller transaction (which becomes rollback-only) and
// partway through a block's first path. Each refusal leaves the Store and the
// arena as before it and consumes neither a drawing id nor a Store id; once
// objects are released the same work is admitted, up to CAP again.
//
// Every expected count comes from the explicit begins, the record tree's
// shape and the records made here, never from a run.
//
// FAIL-BEFORE. The merge base's o1_store.hpp (3ea5a0d6) declares neither
// TxPoolExhausted, Store::scope(), Shape::max_live_objects nor the four hooks
// history_drawing_store_fixture.hpp overrides, so this unit does not compile
// against that header closure (a feature diagnostic). Behavioural RED: the
// handle-pool and live-object-limit mutation patches.
#include "history_drawing_store_fixture.hpp"
#include <pineforge/detail/scratch_errors.hpp>
#include <pineforge/drawing.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/series.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using history_drawing_test::HandleCounts;
using history_drawing_test::StoreFixture;
using pineforge::DrawingArena;
using pineforge::LabelRec;
using pineforge::LineRec;
using pineforge::XLoc;
using pineforge::YLoc;
using pineforge::detail::ScratchCapacityError;
using pineforge::detail::o1::kMaxLiveObjectsCap;
using pineforge::detail::o1::kMaxTxHandles;
using pineforge::detail::o1::RollbackOnly;
using pineforge::detail::o1::Shape;
using pineforge::detail::o1::Store;
using pineforge::detail::o1::Tx;
using pineforge::detail::o1::TxPoolExhausted;
using History = pineforge::DynamicRingBuffer<double>;

// The message is printed before the throw: a value released inside a
// rolled-back or rollback-only transaction while a failure unwinds can stop
// the process in its destructor, and the row's verdict must still be read.
void require(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "history and drawing store limits: %s\n", message);
    throw std::runtime_error(message);
}

// Caps below one page of ids (4096 / 4) keep the live order inline, and this
// window, above one page of doubles (4096 / 8), keeps the history external.
constexpr int kCap = 200;
constexpr std::size_t kHistory = 600;

// o1_store.hpp: with no transaction open, an operation owns one handle.
constexpr std::uint64_t kLoneOperationHandles = 1;

// The external record tree (detail/drawing_records.hpp): six 32-way node
// levels over blocks of 32 records, two of them decoded per value. The first
// flush of a fresh tree creates its block, then one node per level bottom up;
// a flush of a new block under the owned path creates that block alone. With
// the live order inline, line records are the only Store objects here.
constexpr std::uint64_t kLevels = 6;
constexpr std::uint64_t kPath = 1 + kLevels;
constexpr int kBlock = 32;
// Room for two paths: two arenas each flush one block.
constexpr std::uint64_t kLimit = 2 * kPath;

LineRec line_at(int i) {
    return {i, i + 3, 0.5 * i, -0.25 * i, XLoc::bar_index, i % 2 != 0, i % 3 != 0, true};
}
LabelRec label_at(int i) {
    return {i, 0.75 * i, XLoc::bar_time, static_cast<YLoc>(i % 3),
            std::string(static_cast<std::size_t>(20 + i % 9), static_cast<char>('a' + i % 26)),
            true};
}
double value_at(int i) { return 100.0 + 0.5 * i; }

bool same(const LineRec& a, const LineRec& b) {
    return a.x1 == b.x1 && a.x2 == b.x2 && a.y1 == b.y1 && a.y2 == b.y2 && a.xloc == b.xloc &&
           a.ext_l == b.ext_l && a.ext_r == b.ext_r && a.alive == b.alive;
}
bool same(const LabelRec& a, const LabelRec& b) {
    return a.x == b.x && a.y == b.y && a.xloc == b.xloc && a.yloc == b.yloc && a.text == b.text &&
           a.alive == b.alive;
}

// Nothing is evicted or deleted here: every id is alive and the order is
// every id, oldest first. Reads take the const path, which never unshares.
template<class Rec>
void check_arena(const DrawingArena<Rec>& arena, const std::vector<Rec>& model,
                 const char* message) {
    std::vector<std::int32_t> order;
    for (std::size_t i = 0; i < model.size(); ++i) order.push_back(static_cast<std::int32_t>(i));
    require(arena.size() == static_cast<std::int32_t>(model.size()) && arena.order() == order,
            message);
    for (std::size_t i = 0; i < model.size(); ++i) {
        const auto id = static_cast<std::int32_t>(i);
        require(arena.alive(id) && same(arena.at(id), model[i]), message);
    }
}

void check_history(const History& history, const std::vector<double>& pushed,
                   const char* message) {
    require(history.size() == std::min(pushed.size(), kHistory) &&
            history.capacity() == kHistory, message);
    for (std::size_t offset = 0; offset < history.size(); ++offset)
        require(history[offset] == pushed[pushed.size() - 1 - offset], message);
}

void fifth_nested_begin_refused() {
    StoreFixture fixture;
    HandleCounts& handles = fixture.handles;
    const Store& real = *fixture.real;
    {
        DrawingArena<LineRec> lines(kCap, fixture.storage);
        DrawingArena<LabelRec> labels(kCap, fixture.storage);
        History history(kHistory, fixture.storage);
        std::vector<LineRec> line_model;
        std::vector<LabelRec> label_model;
        std::vector<double> pushed;
        // Every value outlives the caller's handles below. On a failure the
        // handles roll the caller transaction back first; a value destroyed
        // inside a rollback-only transaction would meet RollbackOnly in its
        // release, which a destructor cannot report.
        std::optional<DrawingArena<LineRec>> saved;
        // Lone operations: with no caller transaction, each owns one handle.
        for (int i = 0; i < 70; ++i) {
            require(lines.alloc(line_at(i)) == i, "DRAWING_STORE_POOL_RED: a line id was not the next one");
            line_model.push_back(line_at(i));
        }
        for (int i = 0; i < 40; ++i) {
            require(labels.alloc(label_at(i)) == i, "DRAWING_STORE_POOL_RED: a label id was not the next one");
            label_model.push_back(label_at(i));
        }
        for (int i = 0; i < 550; ++i) {
            history.push_front(value_at(i));
            pushed.push_back(value_at(i));
        }
        (void)lines.storage_stats();
        (void)labels.storage_stats();
        (void)history.storage_stats();
        require(real.stats().max_live_handles == kLoneOperationHandles && handles.live == 0 &&
                handles.begins > 0 && handles.joined == 0,
                "DRAWING_STORE_POOL_RED: a lone drawing operation did not own exactly one handle");

        // The caller takes every pool handle: one outer scope, the rest joined.
        // caller_handles counts the explicit begins the caller holds at once.
        std::array<std::unique_ptr<Tx>, kMaxTxHandles> caller{};
        std::uint64_t caller_handles = 0;
        for (auto& handle : caller) {
            handle = fixture.storage->store->begin();
            ++caller_handles;
        }
        require(caller_handles == Store::max_caller_handles() &&
                real.stats().max_live_handles == caller_handles && handles.live == caller_handles,
                "DRAWING_STORE_POOL_RED: the caller does not hold every pool handle");
        // Each call below opens exactly one drawing operation: one dirty
        // working block flushed, or one root retained by a copy or restore.
        const auto joined_operation = [&](auto&& call) {
            const auto joins = handles.joined;
            const auto begins = handles.begins;
            try {
                call();
            } catch (const TxPoolExhausted&) {
                require(false, "DRAWING_STORE_POOL_RED: a drawing operation took a handle from "
                               "the full caller pool");
            }
            require(handles.joined == joins + 1 && handles.begins == begins &&
                    real.stats().max_live_handles == caller_handles &&
                    handles.live == caller_handles,
                    "DRAWING_STORE_POOL_RED: a drawing operation did not join the caller transaction");
        };
        lines.at(5).y1 = -5.5;
        line_model[5].y1 = -5.5;
        joined_operation([&] { (void)lines.storage_stats(); });
        require(labels.alloc(label_at(40)) == 40, "DRAWING_STORE_POOL_RED: a label id was not the next one");
        label_model.push_back(label_at(40));
        joined_operation([&] { (void)labels.storage_stats(); });
        history.push_front(value_at(550));
        pushed.push_back(value_at(550));
        joined_operation([&] { (void)history.storage_stats(); });

        // One more nested begin() is refused before it changes anything.
        const auto before = real.stats();
        const auto begins = handles.begins;
        bool exhausted = false;
        try {
            auto fifth = fixture.storage->store->begin();
        } catch (const TxPoolExhausted&) {
            exhausted = true;
        }
        require(exhausted,
                "DRAWING_STORE_POOL_RED: a nested explicit begin() beyond the caller pool was admitted");
        const auto after = real.stats();
        require(after.objects_live == before.objects_live && after.bytes_live == before.bytes_live &&
                after.max_live_handles == caller_handles && handles.live == caller_handles &&
                handles.begins == begins,
                "DRAWING_STORE_POOL_RED: the refused begin() changed the Store or took a handle");
        check_arena(lines, line_model, "DRAWING_STORE_POOL_RED: the refused begin() changed a line");
        check_arena(labels, label_model, "DRAWING_STORE_POOL_RED: the refused begin() changed a label");
        check_history(history, pushed, "DRAWING_STORE_POOL_RED: the refused begin() changed the history");

        // The caller's transaction stays usable: a checkpoint, a speculative
        // edit and its restore, a joined label write, then the innermost
        // handle returns its slot and the next begin() takes it again.
        try {
            joined_operation([&] { saved.emplace(lines); });
            lines.at(7).x2 = 777;
            joined_operation([&] { lines = *saved; });
            labels.at(3).text = "joined after the refusal";
            label_model[3].text = "joined after the refusal";
            joined_operation([&] { (void)labels.storage_stats(); });
            caller.back()->commit();
            caller.back().reset();
            caller.back() = fixture.storage->store->begin();
            require(real.stats().max_live_handles == caller_handles && handles.live == caller_handles,
                    "DRAWING_STORE_POOL_RED: the refused begin() consumed a pool slot");
            for (std::size_t i = caller.size(); i-- > 1;) caller[i]->commit();
            caller.front()->commit();
        } catch (const RollbackOnly&) {
            require(false, "DRAWING_STORE_POOL_RED: the refused begin() made the caller transaction "
                           "rollback-only");
        }
        for (auto& handle : caller) handle.reset();
        require(handles.live == 0 && handles.abandoned == 0 &&
                real.stats().max_live_handles == caller_handles,
                "DRAWING_STORE_POOL_RED: the pool row left a handle live or exceeded the explicit begins");

        // The commit kept every joined change; the restore dropped the edit.
        check_arena(lines, line_model, "DRAWING_STORE_POOL_RED: the caller commit lost a line change");
        check_arena(*saved, line_model, "DRAWING_STORE_POOL_RED: the checkpoint changed");
        check_arena(labels, label_model, "DRAWING_STORE_POOL_RED: the caller commit lost a label change");
        check_history(history, pushed, "DRAWING_STORE_POOL_RED: the caller commit lost a history value");
    }
    require(real.stats().objects_live == 0 && real.stats().bytes_live == 0,
            "DRAWING_STORE_POOL_RED: the pool row retained Store objects");
    std::printf("history and drawing store limits: pool=%zu max_live_handles=%llu "
                "fifth begin refused\n",
                Store::max_caller_handles(),
                static_cast<unsigned long long>(real.stats().max_live_handles));
}

// Independent oracle for the Store's live-object refusal text: the digits are
// grouped here, never by the production formatter or message builder.
std::string grouped(std::uint64_t value) {
    std::string text = std::to_string(value);
    for (std::size_t end = text.size(); end > 3; end -= 3) text.insert(end - 3, 1, ',');
    return text;
}
std::string object_limit_text(std::uint64_t limit) {
    return "more than " + grouped(limit) + " store objects live at once; " + grouped(limit) +
           " is the most allowed in this store; attempted live count " + grouped(limit + 1) +
           ", approved maximum " + grouped(kMaxLiveObjectsCap);
}

// The call must try to create one object past the limit. It must throw the
// Store's typed cohort refusal and leave the Store's live objects and bytes
// as they were before the call.
template<class Call>
void refused_at_limit(const Store& real, const char* admitted_message, Call&& call) {
    const auto before = real.stats();
    bool refused = false;
    try {
        call();
    } catch (const ScratchCapacityError& error) {
        refused = true;
        const auto failure = pineforge::classify_run_failure(error);
        require(failure.code == pineforge::RunFailureCode::scratch_capacity_exceeded &&
                failure.args != nullptr && *failure.args == "{\"resource\":\"cohort\"}" &&
                std::string(error.what()) == object_limit_text(kLimit),
                "DRAWING_STORE_LIMIT_RED: CAP+1 refusal lost its cohort argument or text");
    } catch (const std::exception&) {
        require(false, "DRAWING_STORE_LIMIT_RED: CAP+1 refusal is not the Store's typed "
                       "ScratchCapacityError");
    }
    require(refused, admitted_message);
    const auto after = real.stats();
    require(after.objects_live == before.objects_live && after.bytes_live == before.bytes_live,
            "DRAWING_STORE_LIMIT_RED: CAP+1 refusal changed the Store");
}

void live_object_cap() {
    // The drawing family's sizes (src/drawing_storage.cpp) with a small
    // configured limit, far below the cap, so CAP and CAP+1 stay cheap.
    Shape shape;
    shape.resident_pages = 8;
    shape.journal_slots = 4;
    shape.index_slots = 4;
    shape.max_live_objects = kLimit;
    StoreFixture fixture(shape);
    HandleCounts& handles = fixture.handles;
    const Store& real = *fixture.real;
    require(real.stats().max_live_objects == kLimit && real.stats().objects_live == 0,
            "DRAWING_STORE_LIMIT_RED: the configured live-object limit was not applied");
    std::uint64_t objects = 0;
    const auto objects_are = [&](const char* message) {
        require(real.stats().objects_live == objects, message);
    };
    std::optional<DrawingArena<LineRec>> first, second;
    std::vector<LineRec> first_model, second_model;
    // Records of the second arena are generated from a different base.
    constexpr int kSecondBase = 1000;
    const auto admitted_alloc = [&](DrawingArena<LineRec>& arena, std::vector<LineRec>& model,
                                    int base) {
        const auto id = static_cast<std::int32_t>(model.size());
        std::int32_t got = -1;
        try {
            got = arena.alloc(line_at(base + id));
        } catch (const ScratchCapacityError&) {
            require(false, "DRAWING_STORE_LIMIT_RED: CAP: an allocation the limit admits was refused");
        }
        require(got == id, "DRAWING_STORE_LIMIT_RED: an admitted allocation did not get the next id");
        model.push_back(line_at(base + id));
    };
    const auto admitted_flush = [&](const DrawingArena<LineRec>& arena) {
        try {
            (void)arena.storage_stats();
        } catch (const ScratchCapacityError&) {
            require(false, "DRAWING_STORE_LIMIT_RED: CAP: a flush the limit admits was refused");
        }
    };

    // CAP. Records in the two working blocks reach no Store object; the
    // third block's first record evicts the oldest block, whose first flush
    // creates one path. A second arena's first flush creates its own.
    first.emplace(kCap, fixture.storage);
    second.emplace(kCap, fixture.storage);
    for (int i = 0; i < 2 * kBlock; ++i) admitted_alloc(*first, first_model, 0);
    objects_are("DRAWING_STORE_LIMIT_RED: records in the working blocks reached the Store");
    admitted_alloc(*first, first_model, 0);
    objects += kPath;
    objects_are("DRAWING_STORE_LIMIT_RED: the first flushed block did not create exactly one path");
    while (first_model.size() < static_cast<std::size_t>(3 * kBlock))
        admitted_alloc(*first, first_model, 0);
    admitted_alloc(*second, second_model, kSecondBase);
    admitted_flush(*second);
    objects += kPath;
    require(objects == kLimit && real.stats().objects_live == kLimit,
            "DRAWING_STORE_LIMIT_RED: CAP: the Store did not reach its live-object limit");

    // CAP+1 from a lone allocation: the next block of the first arena needs
    // one more object. Its own scope rolls back; no id is consumed.
    const auto refused_id = static_cast<std::int32_t>(first_model.size());
    const auto last_admitted = handles.last_id;
    refused_at_limit(real, "DRAWING_STORE_LIMIT_RED: CAP+1: a lone line allocation past the limit "
                           "was admitted",
                     [&] { (void)first->alloc(line_at(refused_id)); });
    require(first->size() == refused_id && first->order().size() == first_model.size() &&
            handles.last_id == last_admitted,
            "DRAWING_STORE_LIMIT_RED: the refused allocation consumed an id or changed the arena");

    // CAP+1 inside a caller transaction: the joined scope that saw the
    // refusal makes the caller rollback-only.
    {
        auto outer = fixture.storage->store->begin();
        const auto abandoned = handles.abandoned;
        refused_at_limit(real, "DRAWING_STORE_LIMIT_RED: CAP+1: a joined line allocation past the "
                               "limit was admitted",
                         [&] { (void)first->alloc(line_at(refused_id)); });
        require(handles.abandoned == abandoned + 1,
                "DRAWING_STORE_LIMIT_RED: the joined scope that saw the refusal did not abandon "
                "the caller transaction");
        bool rollback_only = false;
        try {
            outer->commit();
        } catch (const RollbackOnly&) {
            rollback_only = true;
        }
        require(rollback_only, "DRAWING_STORE_LIMIT_RED: a refusal inside a joined drawing "
                               "operation left the caller committable");
    }
    require(first->size() == refused_id && handles.last_id == last_admitted && handles.live == 0 &&
            real.stats().objects_live == kLimit,
            "DRAWING_STORE_LIMIT_RED: the refused caller transaction changed the arena or the Store");

    // Releasing the second arena frees its path. No refusal consumed an id:
    // the allocation gets the refused drawing id, and its new block the
    // Store id after the last admitted one.
    second.reset();
    second_model.clear();
    objects -= kPath;
    objects_are("DRAWING_STORE_LIMIT_RED: releasing an arena did not free exactly its path");
    admitted_alloc(*first, first_model, 0);
    objects += 1;
    objects_are("DRAWING_STORE_LIMIT_RED: a new block under the owned path did not create one object");
    require(handles.last_id == last_admitted + 1,
            "DRAWING_STORE_LIMIT_RED: a refused creation consumed a Store id");
    // The two dirty working blocks flush as two new blocks under the path.
    admitted_flush(*first);
    objects += 2;
    objects_are("DRAWING_STORE_LIMIT_RED: flushing two new blocks did not create two objects");
    check_arena(*first, first_model, "DRAWING_STORE_LIMIT_RED: a refusal changed a line record");

    // CAP+1 partway through a path: a fresh arena's first flush needs a whole
    // path and fewer objects are free. Its own scope rolls back the part it
    // made; those admitted objects keep their ids, the refused one takes none.
    second.emplace(kCap, fixture.storage);
    admitted_alloc(*second, second_model, kSecondBase);
    objects_are("DRAWING_STORE_LIMIT_RED: a record in a working block reached the Store");
    const std::uint64_t room = kLimit - objects;
    require(room > 0 && room < kPath,
            "DRAWING_STORE_LIMIT_RED: the partial-path refusal needs room for part of a path");
    const auto before_path = handles.last_id;
    refused_at_limit(real, "DRAWING_STORE_LIMIT_RED: CAP+1: a flush past the limit was admitted",
                     [&] { (void)second->storage_stats(); });
    require(handles.last_id == before_path + room && second->size() == 1,
            "DRAWING_STORE_LIMIT_RED: the partial path made other objects than the room admits");

    // CAP again: with the first arena released the same flush fits, and a
    // fresh first arena's path reaches the limit exactly.
    first.reset();
    first_model.clear();
    objects = 0;
    objects_are("DRAWING_STORE_LIMIT_RED: releasing the first arena did not free every object");
    admitted_flush(*second);
    objects += kPath;
    require(handles.last_id == before_path + room + kPath,
            "DRAWING_STORE_LIMIT_RED: a refused creation consumed a Store id");
    first.emplace(kCap, fixture.storage);
    admitted_alloc(*first, first_model, 0);
    admitted_flush(*first);
    objects += kPath;
    require(objects == kLimit && real.stats().objects_live == kLimit,
            "DRAWING_STORE_LIMIT_RED: CAP: the Store did not reach its live-object limit again");
    check_arena(*first, first_model, "DRAWING_STORE_LIMIT_RED: a line record admitted at CAP changed");
    check_arena(*second, second_model, "DRAWING_STORE_LIMIT_RED: the refused flush changed its record");
    first.reset();
    second.reset();
    require(real.stats().objects_live == 0 && real.stats().bytes_live == 0,
            "DRAWING_STORE_LIMIT_RED: the limit row retained Store objects");
    std::printf("history and drawing store limits: live_object_limit=%llu CAP admitted; CAP+1 "
                "refused lone, joined and partway through a path\n",
                static_cast<unsigned long long>(kLimit));
}
} // namespace

int main() {
    // The limit row runs first. It never fills the handle pool, so a pool or
    // joining regression first fails the pool row's own checks, while a limit
    // or atomicity regression fails the limit row. Both rows always run.
    int failures = 0;
    const auto row = [&](const char* name, void (*body)()) {
        try {
            body();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "history and drawing store limits: %s row failed: %s\n", name,
                         error.what());
            ++failures;
        }
    };
    row("live_object_limit", live_object_cap);
    row("fifth_nested_begin", fifth_nested_begin_refused);
    if (failures == 0)
        std::puts("history and drawing store limits: live-object limit and handle pool rows passed");
    return failures == 0 ? 0 : 1;
}
