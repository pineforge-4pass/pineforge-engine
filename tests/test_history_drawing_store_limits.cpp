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
// Footprint rows. A capacity-600 history ring pushed 1,000,000 times keeps a
// Store footprint (objects, logical bytes, backing extent) bounded by its
// capacity; label text takes no Store object of its own, over 1,064,960
// labels created and evicted or deleted at a cap of 50, and the typed
// refusal still comes when the record tree itself passes the limit.
//
// Atomicity rows. A refused allocation that must evict at the cap leaves the
// oldest drawing live and in *.all. A release that the Store refuses (a
// rollback-only caller transaction) propagates from an assignment, leaving
// the value as it was, and a destructor records it instead of stopping the
// process.
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
#include <deque>
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
void refused_at_limit(const Store& real, const char* admitted_message, Call&& call,
                      std::uint64_t limit = kLimit) {
    const auto before = real.stats();
    bool refused = false;
    try {
        call();
    } catch (const ScratchCapacityError& error) {
        refused = true;
        const auto failure = pineforge::classify_run_failure(error);
        require(failure.code == pineforge::RunFailureCode::scratch_capacity_exceeded &&
                failure.args != nullptr && *failure.args == "{\"resource\":\"cohort\"}" &&
                std::string(error.what()) == object_limit_text(limit),
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

// ---- footprint rows ---------------------------------------------------------
constexpr std::uint64_t kSlotsPerBlock = 32;
constexpr std::uint64_t kPageBytes = 4096;

// The record store's backing extent in pages, from its design
// (detail/scratch_store_impl.hpp), for at most `records` Store objects live at
// once, each one record: a 16-byte key and a value of `value_bytes` (the
// object's 16-byte header and its data, one chunk, at most half a page). A
// 4096-byte page keeps 4080 usable bytes. A key or value up to half a page is
// a chain of 64-byte cells carrying 56 bytes each, 63 cells to a page, and
// cell_alloc and alloc_page take their free lists first, so space an erase or
// a replacement frees is reused at once: the cell pages never exceed the peak
// of live cells, plus one record's (an upsert writes before it frees). The
// ordered index of at most 506 handles is one leaf. The handle table (127
// entries a leaf, 508 children a node, three levels below 32,774,128 handles)
// keeps at most one leaf and one node per live record, a root, and at most two
// roots left behind as it grew. A transaction journals a page it changes once,
// into pages reused across transactions, so the journal never needs more
// pages than the data. Two superblocks.
constexpr std::uint64_t backing_pages_for(std::uint64_t records, std::uint64_t value_bytes) {
    const std::uint64_t cell_payload = 56;
    const std::uint64_t cells_per_page = 63;
    const std::uint64_t cells_per_record = 1 + (value_bytes + cell_payload - 1) / cell_payload;
    const std::uint64_t cell_pages =
        ((records + 1) * cells_per_record + cells_per_page - 1) / cells_per_page;
    const std::uint64_t index_pages = 1;
    const std::uint64_t handle_pages = 1 + 2 * (records + 1) + 2;
    const std::uint64_t data_pages = cell_pages + index_pages + handle_pages;
    return 2 + 2 * data_pages;
}

// A capacity-600 ring of doubles (detail/history_ring.hpp) addresses its slots
// modulo 1024, the smallest power of two not below 600: 32 blocks of 32 slots.
// Every address is below 32 * 32, so each of the twelve node levels holds one
// node. A block or a node is two 8-byte header words and 32 eight-byte values
// or child ids.
constexpr std::uint64_t kRingSlots = 1024;
constexpr std::uint64_t kRingLevels = 12;
constexpr std::uint64_t kRingObjects = kRingSlots / kSlotsPerBlock + kRingLevels;
constexpr std::uint64_t kRingObjectBytes =
    2 * sizeof(std::uint64_t) + kSlotsPerBlock * sizeof(double);
static_assert(sizeof(double) == sizeof(pineforge::detail::o1::Id),
              "a node's child ids are as wide as the ring's values");
// At most two versions of the tree are live at once (the ring and its
// calc-on-fill copy), and one root-to-block path is being copied.
constexpr std::uint64_t kRingPeakObjects = 2 * kRingObjects + kRingLevels + 1;
constexpr std::uint64_t kRingBackingPages =
    backing_pages_for(kRingPeakObjects, 16 + kRingObjectBytes);
static_assert(kRingObjects == 44 && kRingObjectBytes == 272 && kRingBackingPages == 442,
              "the ring bound is the arithmetic above");

double ring_value(std::size_t n) { return 0.25 * static_cast<double>(n) - 7.0; }

// A capacity-600 history ring pushed 1,000,000 times, with a calc-on-fill
// cycle (a copy, a speculative update and pushes, the restore) every 997
// pushes. Every 100,000 pushes, with no copy alive, its Store holds at most
// kRingObjects objects of kRingObjectBytes each and a backing extent of
// kRingBackingPages pages, whatever the number of pushes. A ring that takes a
// new slot per push and never releases one holds 3,125 blocks and 98 + 4 + 1
// + 9 nodes, 3,237 objects, at the first check.
void history_ring_footprint() {
    StoreFixture fixture;
    const Store& real = *fixture.real;
    constexpr std::size_t kPushes = 1000000;
    constexpr std::size_t kCycle = 997;
    constexpr std::size_t kCheck = 100000;
    std::uint64_t most_objects = 0, most_bytes = 0, most_backing = 0;
    {
        History history(kHistory, fixture.storage);
        for (std::size_t n = 0; n < kPushes; ++n) {
            history.push_front(ring_value(n));
            if ((n + 1) % kCycle == 0) {
                const History saved = history;
                history.update_front(-1.0);
                for (int i = 2; i <= 4; ++i) history.push_front(-static_cast<double>(i));
                history = saved;
                const std::size_t size = std::min(n + 1, kHistory);
                require(history.size() == size && history[0] == ring_value(n) &&
                        history[size - 1] == ring_value(n + 1 - size),
                        "HISTORY_RING_FOOTPRINT_RED: a restore changed the window");
            }
            if ((n + 1) % kCheck != 0) continue;
            const auto stats = history.storage_stats();
            most_objects = std::max(most_objects, stats.objects_live);
            most_bytes = std::max(most_bytes, stats.bytes_live);
            most_backing = std::max(most_backing, stats.bytes_backing);
            require(stats.objects_live <= kRingObjects,
                    "HISTORY_RING_FOOTPRINT_RED: the ring's Store objects grew with its pushes");
            require(stats.bytes_live <= kRingObjects * kRingObjectBytes,
                    "HISTORY_RING_FOOTPRINT_RED: the ring's live Store bytes grew with its pushes");
            require(stats.bytes_backing <= kRingBackingPages * kPageBytes,
                    "HISTORY_RING_FOOTPRINT_RED: the ring's backing grew with its pushes");
        }
        require(history.size() == kHistory && history.capacity() == kHistory,
                "HISTORY_RING_FOOTPRINT_RED: the window's shape changed");
        for (std::size_t offset = 0; offset < kHistory; ++offset)
            require(history[offset] == ring_value(kPushes - 1 - offset),
                    "HISTORY_RING_FOOTPRINT_RED: a late read of the window changed");
        require(pineforge::is_na(history[kHistory]),
                "HISTORY_RING_FOOTPRINT_RED: a read past the window returned a value");
    }
    require(real.stats().objects_live == 0 && real.stats().bytes_live == 0,
            "HISTORY_RING_FOOTPRINT_RED: the ring row retained Store objects");
    std::printf("history and drawing store limits: ring capacity=%zu pushes=%zu objects<=%llu "
                "(bound %llu) live<=%llu (bound %llu) backing<=%llu (bound %llu)\n",
                kHistory, kPushes, static_cast<unsigned long long>(most_objects),
                static_cast<unsigned long long>(kRingObjects),
                static_cast<unsigned long long>(most_bytes),
                static_cast<unsigned long long>(kRingObjects * kRingObjectBytes),
                static_cast<unsigned long long>(most_backing),
                static_cast<unsigned long long>(kRingBackingPages * kPageBytes));
}

// The six-level record tree over `records` records, every block flushed: one
// block per 32 records, then on each node level one node per 32 below it.
constexpr std::uint64_t tree_objects(std::uint64_t records) {
    std::uint64_t below = (records + kSlotsPerBlock - 1) / kSlotsPerBlock;
    std::uint64_t total = below;
    for (std::uint64_t level = 0; level < kLevels; ++level) {
        below = (below + kSlotsPerBlock - 1) / kSlotsPerBlock;
        total += below;
    }
    return total;
}
static_assert(tree_objects(1064960) == 33280 + 1040 + 33 + 2 + 1 + 1 + 1,
              "the record tree over 1,040 x 1,024 labels");
// A label block's object is two header words, its 32 wire records, then the
// texts of its labels; a node is two header words and 32 child ids.
constexpr std::uint64_t kLabelBlockBytes =
    2 * sizeof(std::uint64_t) + kSlotsPerBlock * sizeof(pineforge::detail::DrawingLabelCodec::Wire);
constexpr std::uint64_t kNodeBytes =
    2 * sizeof(std::uint64_t) + kSlotsPerBlock * sizeof(pineforge::detail::o1::Id);

std::string footprint_text(std::size_t i) {
    std::string text(8 + i % 17, '\0');
    for (std::size_t k = 0; k < text.size(); ++k)
        text[k] = static_cast<char>('A' + (i * 7 + k * 3) % 26);
    return text;
}

// 1,064,960 labels with text, created at a cap of 50 (each past the cap evicts
// the oldest) and every seventh deleted at once, so at most 50 are live. A
// deleted or evicted label stays readable, so each keeps its record and its
// text bytes; the text costs no Store object, so the Store holds the record
// tree's objects and no more: one per 32 labels and the nodes above them.
// Labels whose text is an object of its own add one object per label: past
// 1,016,000 labels they need more than the 1,048,576 live objects a Store
// admits, and at the first check (262,144 labels) they hold 270,604 objects
// against a bound of 8,460.
void label_text_footprint() {
    StoreFixture fixture;
    const Store& real = *fixture.real;
    constexpr std::size_t kLabelCap = 50;
    constexpr std::size_t kLabels = 1064960;
    constexpr std::size_t kCheck = 262144;
    std::uint64_t text_bytes = 0, most_objects = 0;
    {
        DrawingArena<LabelRec> labels(static_cast<int>(kLabelCap), fixture.storage);
        std::deque<std::int32_t> order;
        for (std::size_t i = 0; i < kLabels; ++i) {
            const auto id = static_cast<std::int32_t>(i);
            const auto label = pineforge::pf_label_new(labels, id, 0.5 * id, footprint_text(i),
                                                       XLoc::bar_time, YLoc::abovebar);
            text_bytes += 8 + i % 17;
            require(label.id == id, "LABEL_TEXT_FOOTPRINT_RED: a label id was not the next one");
            if (order.size() == kLabelCap) order.pop_front();
            order.push_back(id);
            if (i % 7 == 3) {
                pineforge::pf_label_delete(labels, label);
                order.pop_back();
            }
            if ((i + 1) % kCheck != 0 && i + 1 != kLabels) continue;
            const auto stats = labels.storage_stats();
            const std::uint64_t made = i + 1;
            const std::uint64_t objects = tree_objects(made);
            const std::uint64_t blocks = (made + kSlotsPerBlock - 1) / kSlotsPerBlock;
            most_objects = std::max(most_objects, stats.objects_live);
            require(stats.objects_live <= objects,
                    "LABEL_TEXT_FOOTPRINT_RED: label text took Store objects of its own");
            require(stats.bytes_live <=
                        blocks * kLabelBlockBytes + text_bytes + (objects - blocks) * kNodeBytes,
                    "LABEL_TEXT_FOOTPRINT_RED: the labels' Store bytes exceed their records and texts");
        }
        require(labels.size() == static_cast<std::int32_t>(kLabels) && labels.order() == order,
                "LABEL_TEXT_FOOTPRINT_RED: the label ids or the *.all order changed");
        // Live, deleted and evicted labels read back whole: the first and
        // last 64 and every 4,099th between.
        const auto check_label = [&](std::size_t i) {
            const auto id = static_cast<std::int32_t>(i);
            const bool alive = std::find(order.begin(), order.end(), id) != order.end();
            const LabelRec rec = std::as_const(labels).at(id);
            require(rec.x == id && rec.y == 0.5 * id && rec.xloc == XLoc::bar_time &&
                        rec.yloc == YLoc::abovebar && rec.text == footprint_text(i) &&
                        rec.alive == alive && labels.alive(id) == alive,
                    "LABEL_TEXT_FOOTPRINT_RED: a label read back changed");
        };
        for (std::size_t i = 0; i < 64; ++i) check_label(i);
        for (std::size_t i = 64; i + 64 < kLabels; i += 4099) check_label(i);
        for (std::size_t i = kLabels - 64; i < kLabels; ++i) check_label(i);
    }
    require(real.stats().objects_live == 0 && real.stats().bytes_live == 0,
            "LABEL_TEXT_FOOTPRINT_RED: the label row retained Store objects");
    std::printf("history and drawing store limits: labels=%zu cap=%zu objects<=%llu (tree %llu) "
                "text_bytes=%llu\n",
                kLabels, kLabelCap, static_cast<unsigned long long>(most_objects),
                static_cast<unsigned long long>(tree_objects(kLabels)),
                static_cast<unsigned long long>(text_bytes));
}

// The live-object refusal still comes, at the record tree's own shape. With
// the texts inside their block, labels reach the Store only as blocks: the
// two working blocks none, the first flushed block its whole path, each later
// block under the same level-1 node one object. At the limit kLimit that
// admits 1 + (kLimit - kPath) flushed blocks and two working ones; the next
// block's flush is the CAP+1 object. Labels whose texts are objects of their
// own need 32 more at the first flush and are refused there.
void label_text_limit() {
    Shape shape;
    shape.resident_pages = 8;
    shape.journal_slots = 4;
    shape.index_slots = 4;
    shape.max_live_objects = kLimit;
    StoreFixture fixture(shape);
    HandleCounts& handles = fixture.handles;
    const Store& real = *fixture.real;
    // Room for every label: an inline roster of 400 ids, so nothing is evicted.
    constexpr int kRoster = 400;
    constexpr std::uint64_t kFlushed = 1 + (kLimit - kPath);
    constexpr int kAdmitted = static_cast<int>((kFlushed + 2) * kSlotsPerBlock);
    static_assert(kAdmitted == 320 && kAdmitted < kRoster, "ten blocks of labels, none evicted");
    std::vector<LabelRec> model;
    {
        DrawingArena<LabelRec> labels(kRoster, fixture.storage);
        for (int i = 0; i < kAdmitted; ++i) {
            std::int32_t got = -1;
            try {
                got = labels.alloc(label_at(i));
            } catch (const ScratchCapacityError&) {
                require(false, "LABEL_TEXT_LIMIT_RED: CAP: a label the limit admits was refused");
            }
            require(got == i, "LABEL_TEXT_LIMIT_RED: an admitted label did not get the next id");
            model.push_back(label_at(i));
        }
        require(real.stats().objects_live == kLimit,
                "LABEL_TEXT_LIMIT_RED: CAP: the labels did not reach the limit at the record "
                "tree's shape");
        // The refused object is the new block itself, the flush's first create.
        const auto refused_id = kAdmitted;
        const auto last_admitted = handles.last_id;
        refused_at_limit(real, "LABEL_TEXT_LIMIT_RED: CAP+1: a label block past the limit was "
                               "admitted",
                         [&] { (void)labels.alloc(label_at(refused_id)); });
        require(labels.size() == kAdmitted && labels.order().size() == model.size() &&
                handles.last_id == last_admitted,
                "LABEL_TEXT_LIMIT_RED: the refused label consumed an id or changed the order");
        // The two working blocks, read without a Store operation.
        for (int i = kAdmitted - 2 * kBlock; i < kAdmitted; ++i)
            require(labels.alive(i) &&
                        same(std::as_const(labels).at(i), model[static_cast<std::size_t>(i)]),
                    "LABEL_TEXT_LIMIT_RED: the refusal changed a label");
    }
    require(real.stats().objects_live == 0 && real.stats().bytes_live == 0,
            "LABEL_TEXT_LIMIT_RED: the label limit row retained Store objects");
    std::printf("history and drawing store limits: label limit=%llu admitted=%d labels in %llu "
                "objects; the next block refused\n",
                static_cast<unsigned long long>(kLimit), kAdmitted,
                static_cast<unsigned long long>(kLimit));
}

// ---- atomicity rows ---------------------------------------------------------
// At the cap an allocation evicts the oldest drawing. Here its new record's
// block must first flush a working block, the first path, past a limit below
// one path: the refusal must leave the oldest drawing live and in *.all, and
// every record as it was. Evicting before the append leaves it dead and gone
// from the order.
void eviction_refused_at_limit() {
    constexpr std::uint64_t kEvictLimit = kPath - 1;
    Shape shape;
    shape.resident_pages = 8;
    shape.journal_slots = 4;
    shape.index_slots = 4;
    shape.max_live_objects = kEvictLimit;
    StoreFixture fixture(shape);
    const Store& real = *fixture.real;
    constexpr int kEvictCap = 3;
    {
        DrawingArena<LineRec> lines(kEvictCap, fixture.storage);
        // Ids 0 .. 63 fill the two working blocks; no block reaches the Store.
        for (int i = 0; i < 2 * kBlock; ++i)
            require(lines.alloc(line_at(i)) == i,
                    "DRAWING_EVICTION_REFUSAL_RED: a line id was not the next one");
        require(real.stats().objects_live == 0,
                "DRAWING_EVICTION_REFUSAL_RED: a working block reached the Store");
        const std::vector<std::int32_t> live{2 * kBlock - 3, 2 * kBlock - 2, 2 * kBlock - 1};
        require(lines.order() == live, "DRAWING_EVICTION_REFUSAL_RED: the FIFO order is not the newest three");
        // Id 64 evicts id 61; its block, the third, needs a working block
        // flushed into the Store first.
        refused_at_limit(real, "DRAWING_EVICTION_REFUSAL_RED: an eviction past the limit was admitted",
                         [&] { (void)lines.alloc(line_at(2 * kBlock)); }, kEvictLimit);
        require(lines.size() == 2 * kBlock && lines.order() == live,
                "DRAWING_EVICTION_REFUSAL_RED: the refused allocation evicted the oldest drawing");
        for (int i = 0; i < 2 * kBlock; ++i) {
            const bool alive = i >= 2 * kBlock - kEvictCap;
            LineRec want = line_at(i);
            want.alive = alive;
            require(lines.alive(i) == alive && same(std::as_const(lines).at(i), want),
                    "DRAWING_EVICTION_REFUSAL_RED: the refused allocation changed a line record");
        }
    }
    require(real.stats().objects_live == 0 && real.stats().bytes_live == 0,
            "DRAWING_EVICTION_REFUSAL_RED: the eviction row retained Store objects");
    std::printf("history and drawing store limits: eviction at cap %d refused at limit %llu; "
                "the oldest drawing stayed live\n",
                kEvictCap, static_cast<unsigned long long>(kEvictLimit));
}

// A rollback-only caller transaction refuses every Store mutation. An
// assignment whose replaced tree must be released then raises RollbackOnly
// and leaves both values as they were; a value destroyed meanwhile records
// one skipped cleanup. Releasing outside a scope, under a catch that
// terminates, stops the process at the first of these.
void release_refusal_propagates() {
    StoreFixture fixture;
    const Store& real = *fixture.real;
    History history(kHistory, fixture.storage), moved(kHistory, fixture.storage);
    std::optional<History> doomed(std::in_place, kHistory, fixture.storage);
    DrawingArena<LineRec> lines(kCap, fixture.storage);
    const DrawingArena<LineRec> empty(kCap, fixture.storage);
    std::vector<LineRec> line_model;
    for (int i = 0; i < 100; ++i) {
        history.push_front(value_at(i));
        moved.push_front(value_at(1000 + i));
        doomed->push_front(value_at(2000 + i));
    }
    for (int i = 0; i < 70; ++i) {
        require(lines.alloc(line_at(i)) == i, "RELEASE_REFUSAL_RED: a line id was not the next one");
        line_model.push_back(line_at(i));
    }
    // Every value reaches the Store: four roots.
    (void)history.storage_stats();
    (void)moved.storage_stats();
    (void)doomed->storage_stats();
    (void)lines.storage_stats();
    const auto skipped = real.stats().cleanup_skipped_rollback_only;
    const auto check_history_values = [&](const History& ring, int base, const char* message) {
        require(ring.size() == 100, message);
        for (std::size_t offset = 0; offset < 100; ++offset)
            require(ring[offset] == value_at(base + 99 - static_cast<int>(offset)), message);
    };
    {
        auto outer = fixture.storage->store->begin();
        {
            auto abandoned = fixture.storage->store->scope();
        } // destroyed unfinished: the caller transaction is rollback-only
        bool refused = false;
        try {
            history = std::move(moved);
        } catch (const RollbackOnly&) {
            refused = true;
        }
        require(refused, "RELEASE_REFUSAL_RED: a history move-assign in a rollback-only "
                         "transaction did not raise RollbackOnly");
        check_history_values(history, 0, "RELEASE_REFUSAL_RED: the refused move changed the receiver");
        check_history_values(moved, 1000, "RELEASE_REFUSAL_RED: the refused move changed its source");
        refused = false;
        try {
            lines = empty;
        } catch (const RollbackOnly&) {
            refused = true;
        }
        require(refused, "RELEASE_REFUSAL_RED: restoring an empty arena in a rollback-only "
                         "transaction did not raise RollbackOnly");
        check_arena(lines, line_model, "RELEASE_REFUSAL_RED: the refused restore changed the arena");
        doomed.reset();
        require(real.stats().cleanup_skipped_rollback_only == skipped + 1,
                "RELEASE_REFUSAL_RED: a value destroyed in a rollback-only transaction did not "
                "record one skipped cleanup");
        outer->rollback();
    }
    // With the caller transaction gone the same assignments complete.
    history = std::move(moved);
    check_history_values(history, 1000, "RELEASE_REFUSAL_RED: the move after the rollback was lost");
    lines = empty;
    require(lines.size() == 0 && lines.order().empty(),
            "RELEASE_REFUSAL_RED: the restore after the rollback was lost");
    std::printf("history and drawing store limits: releases refused by a rollback-only "
                "transaction raised RollbackOnly; one skipped cleanup recorded\n");
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
    row("history_ring_footprint", history_ring_footprint);
    row("label_text_footprint", label_text_footprint);
    row("label_text_limit", label_text_limit);
    row("eviction_refused_at_limit", eviction_refused_at_limit);
    row("release_refusal_propagates", release_refusal_propagates);
    if (failures == 0)
        std::puts("history and drawing store limits: limit, handle pool, footprint and "
                  "atomicity rows passed");
    return failures == 0 ? 0 : 1;
}
