#pragma once
//
// PineForge drawing-objects-as-data runtime.
//
// Spec: docs/drawing-objects-as-data.md §3 ("C++ Runtime").
//
// This is a HEADLESS backtester: drawing objects (line/box/label/linefill) are
// pure DATA the strategy can create, mutate and read back into trading logic.
// There is NO rendering, NO graphics — only geometry. The header is fully
// self-contained (its backing owner is in drawing_storage.cpp); codegen emits
// calls to the `pf_*` free functions below. Nothing here is exported via the C
// ABI — these are codegen-emitted in-process calls, not `c_abi.cpp` symbols.
//
// The header is gated into the generated include block behind `self._uses_drawing`
// exactly like `matrix.hpp` is gated behind `_uses_matrix`. The arenas are members
// of the generated strategy subclass, so reset-per-run is automatic (a fresh
// strategy instance is constructed per backtest).
//
// The arenas are also part of the generated script state that calc_on_order_fills
// checkpoints by value -- TradingView rolls a fill recalculation's drawing edits
// back with the script's variables. The four public record kinds keep every
// record, from the first one, in an external store (detail::DrawingExternalRecords,
// a persistent 32-way tree of 32-record blocks over o1::Store): an arena holds
// two decoded working blocks, a copy shares the tree (O(1), the first copy
// included) and a write after a copy copies one root-to-block path in the
// store. Resident memory is the store's fixed workspace and those blocks,
// whatever the number of records. The live order is a ring prepared from the
// declared cap (external slots above one page of ids). Application-defined
// record types keep the generic heap store below. Label text keeps its
// std::string type; DrawingLabelCodec is the binding point for the string lane.
//
#include <array>
#include <atomic>
#include <vector>
#include <deque>
#include <memory>
#include <new>
#include <string>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <cmath>
#include <type_traits>
#include <utility>
#include <pineforge/na.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/detail/drawing_order.hpp>

namespace pineforge {

// ---- enums (spec §3.2) -----------------------------------------------------
enum class XLoc : int { bar_index = 0, bar_time = 1 };           // default bar_index
enum class YLoc : int { price = 0, abovebar = 1, belowbar = 2 }; // default price

// ---- value-view handles (spec §3.2) ----------------------------------------
// Stored in vars / array<T> / UDT fields. id < 0 == na.
struct Line     { int32_t id = -1; };
struct Box      { int32_t id = -1; };
struct Label    { int32_t id = -1; };
struct Linefill { int32_t id = -1; };

inline bool is_na(const Line& h)     { return h.id < 0; }
inline bool is_na(const Box& h)      { return h.id < 0; }
inline bool is_na(const Label& h)    { return h.id < 0; }
inline bool is_na(const Linefill& h) { return h.id < 0; }

// ---- pure value type: chart.point (spec §3.2) ------------------------------
// No arena, no identity, no delete. Lowered inline as aggregate literals by
// codegen; the engine only needs the struct + its is_na overload.
struct ChartPoint {
    int64_t index = na<int64_t>();   // x-coord as bar index (na sentinel INT64_MIN)
    int64_t time  = na<int64_t>();   // x-coord as UNIX ms
    double  price = na<double>();    // y-coord (NaN sentinel)
};
inline bool is_na(const ChartPoint& p) {
    return is_na(p.index) && is_na(p.time) && is_na(p.price);
}

// ---- arena records (spec §3.3) ---------------------------------------------
// Geometry only — every visual field (color/style/width/...) is dropped at
// lowering. x-coords are int64_t (xloc.bar_time stores UNIX-ms, overflows
// int32; na sentinel INT64_MIN); y-coords/prices are double (NaN sentinel).
// NOTE: BoxRec has NO extend and NO text fields (unlike LineRec / LabelRec).
struct LineRec     { int64_t x1, x2; double y1, y2; XLoc xloc; bool ext_l, ext_r; bool alive; };
struct BoxRec      { int64_t left, right; double top, bottom; XLoc xloc; bool alive; };
struct LabelRec    { int64_t x; double y; XLoc xloc; YLoc yloc; std::string text; bool alive; };
struct LinefillRec { int32_t line1, line2; bool alive; };

// ---- error type (spec §3.7) ------------------------------------------------
// Derives from std::runtime_error so BacktestEngine::run()'s existing
// `catch (const std::exception& e)` (src/engine_run.cpp) halts the backtest
// exactly like TradingView when a drawing op touches a na handle.
// Drawing is ALWAYS-ON and ALWAYS TV-faithful: no compile flag, no lenient mode.
struct pine_drawing_error : std::runtime_error {
    explicit pine_drawing_error(const std::string& msg) : std::runtime_error(msg) {}
};

namespace detail {

// The Pine kind of a drawing record (pine_na_reference's object), known from
// the record type at the call and never read from the script.
template <class Rec>
constexpr const char* drawing_object_name() noexcept {
    if constexpr (std::is_same_v<Rec, LineRec>) return "line";
    else if constexpr (std::is_same_v<Rec, BoxRec>) return "box";
    else if constexpr (std::is_same_v<Rec, LabelRec>) return "label";
    else if constexpr (std::is_same_v<Rec, LinefillRec>) return "linefill";
    else static_assert(!std::is_same_v<Rec, Rec>, "name the drawing kind of this record");
}

// A drawing call the script got wrong, raised as the pine_drawing_error it
// always was (the same English): a na handle is pine_na_reference, a bad
// argument pine_invalid_argument. Out of line and cold, so the accessors that
// check stay as small as they were.
[[noreturn, gnu::noinline, gnu::cold]] inline void drawing_na_error(const char* object,
                                                                   const char* what) {
    throw coded<pine_drawing_error>(RunFailureCode::pine_na_reference,
                                    {{"object", object}}, what);
}
[[noreturn, gnu::noinline, gnu::cold]] inline void drawing_argument_error(
        const char* function, const char* argument, const char* rule, const char* what) {
    throw coded<pine_drawing_error>(
        RunFailureCode::pine_invalid_argument,
        {{"function", function}, {"argument", argument}, {"rule", rule}}, what);
}

// ---- record store: a vector, or blocks shared between copies ----------------
// The records of one DrawingArena, indexed by id. A store starts as a vector,
// exactly as the arena always was, and nothing in it is ever shared: a copy of
// it is built from its records (O(records)). That copy holds them in blocks of
// 32 -- the newest block, the one alloc appends to, beside a radix tree (a
// 32-way trie over the id) that holds every earlier block -- and a copy of a
// block store shares every block, as does a store assigned from one. A write
// first gives the writing store its own copy of every block and tree node on
// the way to the record that another copy still shares, then writes in place,
// so a copy of a block store costs O(1) and a write after it one root-to-block
// path (at most six nodes and a block for 2^31 ids). Every copy stays an
// independent value: a shared block or node is never written.
//
// Under the scheduler's sequence (snapshot, restore, execute, commit) the first
// snapshot copies the vector and the restore assigns the copy back, so from
// the first bar on an arena and its checkpoint share blocks. An arena that is
// never copied stays a vector.
template <class Rec>
class DrawingRecordStore {
    static_assert(std::is_copy_constructible_v<Rec> && std::is_nothrow_move_constructible_v<Rec>,
                  "a drawing record must be copyable and nothrow move-constructible");
    static constexpr unsigned kBits = 5;
    static constexpr std::uint32_t kWidth = std::uint32_t{1} << kBits;
    static constexpr std::uint32_t kMask = kWidth - 1;

    // Up to 32 records, constructed in place in slots [0, used) like a vector's
    // elements; the other slots hold no object. Every block in the tree is full.
    struct Block {
        union Slot {
            Rec rec;
            Slot() noexcept {}
            ~Slot() {}
        };
        std::uint32_t used = 0;
        Slot slots[kWidth];

        Block() noexcept {}
        Block(const Block& other) {
            try {
                for (; used < other.used; ++used) ::new (&slots[used].rec) Rec(other.slots[used].rec);
            } catch (...) {
                clear();
                throw;
            }
        }
        Block& operator=(const Block&) = delete;
        ~Block() { clear(); }

        void clear() noexcept {
            while (used > 0) slots[--used].rec.~Rec();
        }
        void append(Rec&& rec) noexcept {
            ::new (&slots[used].rec) Rec(std::move(rec));
            ++used;
        }
    };
    struct Node { std::array<std::shared_ptr<void>, kWidth> kids{}; };  // a Node below kBits, else a Block

    std::vector<Rec> flat_;         // the records unless blocks_
    std::shared_ptr<Node> root_;    // the blocks before the tail; null until the first block fills
    std::shared_ptr<Block> tail_;   // records tail_first_ .. size_ - 1; null while empty
    std::uint32_t size_ = 0;
    std::uint32_t tail_first_ = 0;  // a multiple of 32
    unsigned shift_ = kBits;        // the root's level: its kids are blocks when shift_ == kBits
    bool blocks_ = false;           // the records are in root_ and tail_, flat_ is empty
    // Blocks only: a copy was taken of this store, or it was copied from one,
    // so its blocks may be shared. Set on both sides by the copy; atomic
    // because copying reads a const store, which threads may do at once.
    mutable std::atomic<bool> shared_{false};

    // The pointee of `slot`, for writing: itself when this store holds its only
    // reference, else a private copy put in its place. The acquire fence pairs
    // with the release decrement of a copy that dropped its reference on another
    // thread, so its reads of the block happen before this store's writes
    // (libstdc++ and libc++ read use_count() as an atomic load of that count).
    template <class T>
    T* own(std::shared_ptr<T>& slot) {
        if (!shared_.load(std::memory_order_relaxed)) return slot.get();
        if (slot.use_count() != 1) slot = std::make_shared<T>(*slot);
        else std::atomic_thread_fence(std::memory_order_acquire);
        return slot.get();
    }
    template <class T>
    T* own_as(std::shared_ptr<void>& slot) {
        if (!shared_.load(std::memory_order_relaxed)) return static_cast<T*>(slot.get());
        if (slot.use_count() != 1) slot = std::make_shared<T>(*static_cast<const T*>(slot.get()));
        else std::atomic_thread_fence(std::memory_order_acquire);
        return static_cast<T*>(slot.get());
    }

    // A chain of nodes `level / kBits` high whose leftmost block is `block`.
    static std::shared_ptr<void> path_to(unsigned level, const std::shared_ptr<Block>& block) {
        std::shared_ptr<void> node = block;
        for (; level > 0; level -= kBits) {
            auto parent = std::make_shared<Node>();
            parent->kids[0] = std::move(node);
            node = std::move(parent);
        }
        return node;
    }

    // A read from blocks: the tail, or the walk from the root. Out of line and
    // cold, like every block path below: what read(), write() and push_back()
    // inline is the vector, the arena's own code before blocks existed.
    [[gnu::noinline, gnu::cold]] const Rec& read_blocks(std::uint32_t id) const {
        if (id >= tail_first_) return tail_->slots[id & kMask].rec;
        const void* node = root_.get();
        for (unsigned level = shift_; level > 0; level -= kBits)
            node = static_cast<const Node*>(node)->kids[(id >> level) & kMask].get();
        return static_cast<const Block*>(node)->slots[id & kMask].rec;
    }

    [[gnu::noinline, gnu::cold]] Rec& write_blocks(std::uint32_t id) {
        if (!shared_.load(std::memory_order_relaxed)) return const_cast<Rec&>(read_blocks(id));
        if (id >= tail_first_) return own(tail_)->slots[id & kMask].rec;
        Node* node = own(root_);
        for (unsigned level = shift_; level > kBits; level -= kBits)
            node = own_as<Node>(node->kids[(id >> level) & kMask]);
        return own_as<Block>(node->kids[(id >> kBits) & kMask])->slots[id & kMask].rec;
    }

    // Appends to blocks: through a tail another copy may share, or when the
    // tail is full or absent, to a new tail block, the full one handed to the
    // tree first.
    [[gnu::noinline, gnu::cold]] void push_block(Rec&& rec) {
        if (size_ != 0 && size_ - tail_first_ < kWidth) {
            own(tail_)->append(std::move(rec));
            ++size_;
            return;
        }
        auto block = std::make_shared<Block>();
        block->append(std::move(rec));
        if (size_ != 0) push_tail();
        tail_ = std::move(block);
        tail_first_ = size_;
        ++size_;
    }
    // Hands the full tail (records size_ - 32 .. size_ - 1) to the tree, growing
    // the tree by a level when it is full. Every allocation precedes the one
    // link that publishes the block, so a throw leaves every record as it was.
    void push_tail() {
        const std::uint32_t last = size_ - 1;
        if (!root_) {
            auto root = std::make_shared<Node>();
            root->kids[0] = tail_;
            root_ = std::move(root);
            shift_ = kBits;
            return;
        }
        if ((std::uint64_t{size_} >> kBits) > (std::uint64_t{1} << shift_)) {
            auto root = std::make_shared<Node>();
            root->kids[1] = path_to(shift_, tail_);
            root->kids[0] = std::move(root_);
            root_ = std::move(root);
            shift_ += kBits;
            return;
        }
        Node* node = own(root_);
        for (unsigned level = shift_; level > kBits; level -= kBits) {
            std::shared_ptr<void>& kid = node->kids[(last >> level) & kMask];
            if (!kid) {
                kid = path_to(level - kBits, tail_);
                return;
            }
            node = own_as<Node>(kid);
        }
        node->kids[(last >> kBits) & kMask] = tail_;
    }

public:
    DrawingRecordStore() = default;
    // A copy of a vector store is built as blocks of its own; a copy of a block
    // store shares its blocks.
    DrawingRecordStore(const DrawingRecordStore& other) : blocks_(true) {
        if (!other.blocks_) {
            for (const Rec& rec : other.flat_) push_block(Rec(rec));
            return;
        }
        other.shared_.store(true, std::memory_order_relaxed);
        root_ = other.root_;
        tail_ = other.tail_;
        size_ = other.size_;
        tail_first_ = other.tail_first_;
        shift_ = other.shift_;
        shared_.store(true, std::memory_order_relaxed);
    }
    DrawingRecordStore& operator=(const DrawingRecordStore& other) {
        if (this != &other) *this = DrawingRecordStore(other);
        return *this;
    }
    // A moved-from store is empty, like the vector it replaced.
    DrawingRecordStore(DrawingRecordStore&& other) noexcept
        : flat_(std::move(other.flat_)), root_(std::move(other.root_)),
          tail_(std::move(other.tail_)), size_(std::exchange(other.size_, 0)),
          tail_first_(std::exchange(other.tail_first_, 0)),
          shift_(std::exchange(other.shift_, kBits)), blocks_(std::exchange(other.blocks_, false)),
          shared_(other.shared_.exchange(false, std::memory_order_relaxed)) {
        other.flat_.clear();
    }
    DrawingRecordStore& operator=(DrawingRecordStore&& other) noexcept {
        if (this != &other) {
            flat_ = std::move(other.flat_);
            other.flat_.clear();
            root_ = std::move(other.root_);
            tail_ = std::move(other.tail_);
            size_ = std::exchange(other.size_, 0);
            tail_first_ = std::exchange(other.tail_first_, 0);
            shift_ = std::exchange(other.shift_, kBits);
            blocks_ = std::exchange(other.blocks_, false);
            shared_.store(other.shared_.exchange(false, std::memory_order_relaxed),
                          std::memory_order_relaxed);
        }
        return *this;
    }

    int32_t size() const { return static_cast<int32_t>(size_); }

    // Both take an id in [0, size()).
    const Rec& read(std::uint32_t id) const {
        if (!blocks_) return flat_[id];
        return read_blocks(id);
    }
    Rec& write(std::uint32_t id) {
        if (!blocks_) return flat_[id];
        return write_blocks(id);
    }

    void push_back(Rec&& rec) {
        if (!blocks_) {
            flat_.push_back(std::move(rec));
            ++size_;
            return;
        }
        push_block(std::move(rec));
    }
};

// Keep the published LabelRec/string signatures. A backing block owns its
// text byte objects until its last version reference disappears; delete/FIFO
// never releases text. A future ScriptString binding replaces this codec,
// LabelRec::text and the legacy getter together, without a second value type.
struct DrawingLabelCodec {
    struct Wire {
        int64_t x = 0;
        double y = 0;
        XLoc xloc = XLoc::bar_index;
        YLoc yloc = YLoc::price;
        o1::Id text = o1::kNullId;
        std::uint64_t text_size = 0;
        bool alive = false;
    };
    static Wire encode(const LabelRec& rec, o1::Store& store) {
        o1::Id text = o1::kNullId;
        if (!rec.text.empty()) {
            text = store.create(rec.text.size(), alignof(char));
            if (store.write(text, 0, rec.text.data(), rec.text.size()) != rec.text.size())
                pine_engine_invariant("short drawing label text write");
        }
        return {rec.x, rec.y, rec.xloc, rec.yloc, text, rec.text.size(), rec.alive};
    }
    static LabelRec decode(const Wire& wire, o1::Store& store) {
        std::string text(static_cast<std::size_t>(wire.text_size), '\0');
        if (!text.empty() && store.read(wire.text, 0, text.data(), text.size()) != text.size())
            pine_engine_invariant("short drawing label text read");
        return {wire.x, wire.y, wire.xloc, wire.yloc, std::move(text), wire.alive};
    }
    static void release(const Wire& wire, o1::Store& store) {
        if (wire.text != o1::kNullId) store.release(wire.text);
    }
};

template<class Rec> struct DrawingStoreFor { using type = DrawingRecordStore<Rec>; };
template<> struct DrawingStoreFor<LineRec> { using type = DrawingExternalRecords<LineRec>; };
template<> struct DrawingStoreFor<BoxRec> { using type = DrawingExternalRecords<BoxRec>; };
template<> struct DrawingStoreFor<LabelRec> {
    using type = DrawingExternalRecords<LabelRec, DrawingLabelCodec>;
};
template<> struct DrawingStoreFor<LinefillRec> { using type = DrawingExternalRecords<LinefillRec>; };

} // namespace detail

// A na handle (id < 0) or an out-of-range id is ALWAYS an error — there is no
// record to read. NOTE: a DEAD record (alive == false, after an explicit
// line.delete() or FIFO eviction) is NOT an error — TV keeps the object's data
// read/writable after delete (the object only leaves the chart + the *.all
// list), so the engine must too. See corpus/validation/drawing-delete-halt,
// where TV keeps producing trades (2504) after the strategy's line.delete().

// ---- arena template (spec §3.4) --------------------------------------------
// ids are monotonic and never reused (dead slots are tombstoned, not recycled)
// -> no ABA, reproducible across runs. Exact-cap FIFO eviction (oldest first).
// A copy shares the records (public kinds: an external root; other kinds: the
// heap store above, once in blocks) and copies the live ring, at most cap ids
// (or retains the ring's external root above one page of ids).
template <class Rec>
class DrawingArena {
    typename detail::DrawingStoreFor<Rec>::type recs_; // stable id, including tombstones
    detail::DrawingOrder order_; // oldest -> newest, exact declared cap
    int                  cap_;
    static auto records_with_storage(const std::shared_ptr<detail::DrawingStorage>& storage) {
        using Records = typename detail::DrawingStoreFor<Rec>::type;
        if constexpr (std::is_constructible_v<Records, std::shared_ptr<detail::DrawingStorage>>)
            return Records(storage);
        else return Records{};
    }
public:
    explicit DrawingArena(int cap = 50)
        : order_(static_cast<std::size_t>(std::max(1, cap))), cap_(std::max(1, cap)) {}
    DrawingArena(const DrawingArena&) = default;
    DrawingArena& operator=(const DrawingArena& other) {
        if (this != &other) {
            order_.prepare_assignment(other.order_);
            auto next_records = other.recs_;
            order_ = other.order_;
            recs_ = std::move(next_records);
            cap_ = other.cap_;
        }
        return *this;
    }
    DrawingArena(int cap, std::shared_ptr<detail::DrawingStorage> storage)
        : recs_(records_with_storage(storage)),
          order_(static_cast<std::size_t>(std::max(1, cap)), std::move(storage)),
          cap_(std::max(1, cap)) {}
    // Prepare the roster before moving records, leaving both values usable
    // if preparation throws. A moved-from arena keeps its cap; self-move
    // preserves the complete value.
    DrawingArena(DrawingArena&& other) : order_(other.order_), cap_(other.cap_) {
        recs_ = std::move(other.recs_);
        other.order_.clear();
    }
    DrawingArena& operator=(DrawingArena&& other) {
        if (this != &other) {
            order_.prepare_assignment(other.order_);
            order_ = other.order_;
            recs_ = std::move(other.recs_);
            other.order_.clear();
            cap_ = other.cap_;
        }
        return *this;
    }

    int32_t alloc(Rec r) {
        auto push = order_.prepare_push();
        while ((int)order_.size() >= cap_) {        // exact-cap FIFO eviction (oldest first)
            // Tombstone before leaving order_: the write may copy a shared block
            // and throw, which must leave the record live and in order_.
            recs_.write(static_cast<std::uint32_t>(order_.front())).alive = false;
            order_.pop_front();
        }
        int32_t id = static_cast<int32_t>(recs_.size());
        r.alive = true;
        recs_.push_back(std::move(r));
        push.commit(id);
        return id;
    }

    void erase(int32_t id) {                         // delete(): silent no-op on na/dead
        if (!alive(id)) return;
        if (order_.back() == id) {
            recs_.write(static_cast<std::uint32_t>(id)).alive = false;
            order_.pop_back();
            return;
        }
        if (order_.is_external()) {
            auto next = order_.erased(std::find(order_.begin(), order_.end(), id));
            recs_.write(static_cast<std::uint32_t>(id)).alive = false;
            order_.swap(next);
            return;
        }
        recs_.write(static_cast<std::uint32_t>(id)).alive = false;
        order_.erase(std::find(order_.begin(), order_.end(), id));
    }

    bool alive(int32_t id) const {
        return id >= 0 && id < size() && recs_.read(static_cast<std::uint32_t>(id)).alive;
    }

    // Number of slots ever allocated (dead slots retained). ids in [0, size())
    // reference a real record (live OR dead); ids outside that range or < 0 are
    // na/invalid.
    int32_t size() const { return static_cast<int32_t>(recs_.size()); }

    template<class Records = typename detail::DrawingStoreFor<Rec>::type>
    auto storage_stats() const -> decltype(std::declval<const Records&>().stats()) {
        auto stats = recs_.stats();
        const auto roster = order_.storage_stats();
        if (recs_.storage_identity() == order_.storage_identity()) return roster;
        stats.bytes_live += roster.bytes_live;
        stats.bytes_resident += roster.bytes_resident;
        stats.pages_spilled += roster.pages_spilled;
        stats.bytes_backing += roster.bytes_backing;
        return stats;
    }

    // Typed working views: consume before the next arena operation, including
    // a read that may replace a working block. Script getters return values.
    Rec&       at(int32_t id)       { return recs_.write(static_cast<std::uint32_t>(id)); }
    const Rec& at(int32_t id) const { return recs_.read(static_cast<std::uint32_t>(id)); }

    const detail::DrawingOrder& order() const { return order_; } // backs *.all
};

// ---- shared helpers --------------------------------------------------------
// Returns a reference to the record for a handle. Only a na handle (id < 0) or
// an out-of-range id throws; a DEAD record (alive == false, e.g. after an
// explicit line.delete() or FIFO eviction) is returned as-is — its geometry
// remains read/writable, matching TV (delete leaves the chart + *.all, not the
// data). See corpus/validation/drawing-delete-halt (2504 post-delete trades).
// Setters take the mutable form; getters the const one, which never unshares.
template <class Rec, class Handle>
inline Rec& pf_require_live(DrawingArena<Rec>& a, Handle h) {
    if (h.id < 0 || h.id >= a.size())
        detail::drawing_na_error(detail::drawing_object_name<Rec>(), "drawing access on na handle");
    return a.at(h.id);
}
template <class Rec, class Handle>
inline const Rec& pf_require_live(const DrawingArena<Rec>& a, Handle h) {
    if (h.id < 0 || h.id >= a.size())
        detail::drawing_na_error(detail::drawing_object_name<Rec>(), "drawing access on na handle");
    return a.at(h.id);
}

// Selects the x-coordinate a ChartPoint contributes for a given xloc.
inline int64_t pf_point_x(const ChartPoint& p, XLoc xloc) {
    return (xloc == XLoc::bar_time) ? p.time : p.index;
}

// ============================ line ==========================================
inline Line pf_line_new(DrawingArena<LineRec>& a, int64_t x1, double y1, int64_t x2, double y2,
                        XLoc xloc = XLoc::bar_index, bool el = false, bool er = false) {
    // *_new(na,na,na,na) allocates a LIVE record with na coords (NOT a na handle).
    return Line{ a.alloc(LineRec{ x1, x2, y1, y2, xloc, el, er, true }) };
}
inline Line pf_line_new_pts(DrawingArena<LineRec>& a, ChartPoint p1, ChartPoint p2,
                            XLoc xloc = XLoc::bar_index) {
    return Line{ a.alloc(LineRec{ pf_point_x(p1, xloc), pf_point_x(p2, xloc),
                                  p1.price, p2.price, xloc, false, false, true }) };
}

inline int64_t pf_line_get_x1(DrawingArena<LineRec>& a, Line h) { return pf_require_live(std::as_const(a), h).x1; }
inline int64_t pf_line_get_x2(DrawingArena<LineRec>& a, Line h) { return pf_require_live(std::as_const(a), h).x2; }
inline double  pf_line_get_y1(DrawingArena<LineRec>& a, Line h) { return pf_require_live(std::as_const(a), h).y1; }
inline double  pf_line_get_y2(DrawingArena<LineRec>& a, Line h) { return pf_require_live(std::as_const(a), h).y2; }

inline void pf_line_set_x1(DrawingArena<LineRec>& a, Line h, int64_t v) { pf_require_live(a, h).x1 = v; }
inline void pf_line_set_x2(DrawingArena<LineRec>& a, Line h, int64_t v) { pf_require_live(a, h).x2 = v; }
inline void pf_line_set_y1(DrawingArena<LineRec>& a, Line h, double v)  { pf_require_live(a, h).y1 = v; }
inline void pf_line_set_y2(DrawingArena<LineRec>& a, Line h, double v)  { pf_require_live(a, h).y2 = v; }
inline void pf_line_set_xy1(DrawingArena<LineRec>& a, Line h, int64_t x, double y) {
    LineRec& r = pf_require_live(a, h); r.x1 = x; r.y1 = y;
}
inline void pf_line_set_xy2(DrawingArena<LineRec>& a, Line h, int64_t x, double y) {
    LineRec& r = pf_require_live(a, h); r.x2 = x; r.y2 = y;
}
inline void pf_line_set_first_point(DrawingArena<LineRec>& a, Line h, ChartPoint p) {
    LineRec& r = pf_require_live(a, h); r.x1 = pf_point_x(p, r.xloc); r.y1 = p.price;
}
inline void pf_line_set_second_point(DrawingArena<LineRec>& a, Line h, ChartPoint p) {
    LineRec& r = pf_require_live(a, h); r.x2 = pf_point_x(p, r.xloc); r.y2 = p.price;
}
inline void pf_line_set_xloc(DrawingArena<LineRec>& a, Line h, int64_t x1, int64_t x2, XLoc xloc) {
    // REINTERPRET: stores new numbers + flag; does NOT convert coord spaces (TV-faithful, spec §3.9).
    LineRec& r = pf_require_live(a, h); r.x1 = x1; r.x2 = x2; r.xloc = xloc;
}

// real computation: infinite line, ignores extend; valid for xloc.bar_index (spec §3.6).
inline double pf_line_get_price(DrawingArena<LineRec>& a, Line h, int64_t x) {
    if (h.id < 0 || h.id >= a.size())            // na/out-of-range line -> throw -> halt (like TV)
        detail::drawing_na_error("line", "drawing access on na handle");
    const LineRec& r = std::as_const(a).at(h.id); // dead record's geometry is still readable (TV-faithful)
    if (r.xloc == XLoc::bar_time)                 // TV errors on bar_time lines
        detail::drawing_argument_error("line.get_price", "xloc", "requires_xloc_bar_index",
                                       "line.get_price requires xloc.bar_index");
    if (r.x1 == r.x2) return na<double>();              // degenerate vertical -> na (matches TV)
    return r.y1 + (r.y2 - r.y1) / (double)(r.x2 - r.x1) * (double)(x - r.x1); // infinite line, ignores extend
}

inline Line pf_line_copy(DrawingArena<LineRec>& a, Line h) {
    if (!a.alive(h.id)) return Line{};                 // copy of na/dead -> na handle (silent)
    LineRec r = std::as_const(a).at(h.id);              // deep copy = independent new arena record
    return Line{ a.alloc(r) };
}
inline void pf_line_delete(DrawingArena<LineRec>& a, Line h) { a.erase(h.id); } // silent no-op on na/dead

// ============================ box ===========================================
inline Box pf_box_new(DrawingArena<BoxRec>& a, int64_t left, double top, int64_t right, double bottom,
                      XLoc xloc = XLoc::bar_index) {
    return Box{ a.alloc(BoxRec{ left, right, top, bottom, xloc, true }) };
}
inline Box pf_box_new_pts(DrawingArena<BoxRec>& a, ChartPoint top_left, ChartPoint bottom_right,
                          XLoc xloc = XLoc::bar_index) {
    return Box{ a.alloc(BoxRec{ pf_point_x(top_left, xloc), pf_point_x(bottom_right, xloc),
                                top_left.price, bottom_right.price, xloc, true }) };
}

inline int64_t pf_box_get_left  (DrawingArena<BoxRec>& a, Box h) { return pf_require_live(std::as_const(a), h).left; }
inline int64_t pf_box_get_right (DrawingArena<BoxRec>& a, Box h) { return pf_require_live(std::as_const(a), h).right; }
inline double  pf_box_get_top   (DrawingArena<BoxRec>& a, Box h) { return pf_require_live(std::as_const(a), h).top; }
inline double  pf_box_get_bottom(DrawingArena<BoxRec>& a, Box h) { return pf_require_live(std::as_const(a), h).bottom; }

inline void pf_box_set_left  (DrawingArena<BoxRec>& a, Box h, int64_t v) { pf_require_live(a, h).left = v; }
inline void pf_box_set_right (DrawingArena<BoxRec>& a, Box h, int64_t v) { pf_require_live(a, h).right = v; }
inline void pf_box_set_top   (DrawingArena<BoxRec>& a, Box h, double v)  { pf_require_live(a, h).top = v; }
inline void pf_box_set_bottom(DrawingArena<BoxRec>& a, Box h, double v)  { pf_require_live(a, h).bottom = v; }
inline void pf_box_set_lefttop(DrawingArena<BoxRec>& a, Box h, int64_t left, double top) {
    BoxRec& r = pf_require_live(a, h); r.left = left; r.top = top;
}
inline void pf_box_set_rightbottom(DrawingArena<BoxRec>& a, Box h, int64_t right, double bottom) {
    BoxRec& r = pf_require_live(a, h); r.right = right; r.bottom = bottom;
}
inline void pf_box_set_top_left_point(DrawingArena<BoxRec>& a, Box h, ChartPoint p) {
    BoxRec& r = pf_require_live(a, h); r.left = pf_point_x(p, r.xloc); r.top = p.price;
}
inline void pf_box_set_bottom_right_point(DrawingArena<BoxRec>& a, Box h, ChartPoint p) {
    BoxRec& r = pf_require_live(a, h); r.right = pf_point_x(p, r.xloc); r.bottom = p.price;
}
inline void pf_box_set_xloc(DrawingArena<BoxRec>& a, Box h, int64_t left, int64_t right, XLoc xloc) {
    BoxRec& r = pf_require_live(a, h); r.left = left; r.right = right; r.xloc = xloc;  // reinterpret only
}

inline Box pf_box_copy(DrawingArena<BoxRec>& a, Box h) {
    if (!a.alive(h.id)) return Box{};
    BoxRec r = std::as_const(a).at(h.id);
    return Box{ a.alloc(r) };
}
inline void pf_box_delete(DrawingArena<BoxRec>& a, Box h) { a.erase(h.id); }

// ============================ label =========================================
// NOTE: yloc.abovebar / yloc.belowbar should auto-place y at the bar high/low
// (spec §3.9). The spec's pf_label_new signature is intentionally decoupled from
// the engine and does NOT receive bar high/low, so y is stored AS-IS and yloc is
// store-only here. yloc abovebar/belowbar auto-place needs bar high/low at the
// call site — codegen would pass close()/high()/low() if ever required. Inert:
// no corpus label reads get_y back (spec note 4), so no parity claim.
inline Label pf_label_new(DrawingArena<LabelRec>& a, int64_t x, double y, std::string text,
                          XLoc xloc = XLoc::bar_index, YLoc yloc = YLoc::price) {
    return Label{ a.alloc(LabelRec{ x, y, xloc, yloc, std::move(text), true }) };
}
inline Label pf_label_new_pt(DrawingArena<LabelRec>& a, ChartPoint point, std::string text,
                             YLoc yloc = YLoc::price) {
    // xloc carried by the ChartPoint: bar_time only when index is na but time is set.
    XLoc xloc = (is_na(point.index) && !is_na(point.time)) ? XLoc::bar_time : XLoc::bar_index;
    return Label{ a.alloc(LabelRec{ pf_point_x(point, xloc), point.price, xloc, yloc,
                                    std::move(text), true }) };
}

inline int64_t     pf_label_get_x(DrawingArena<LabelRec>& a, Label h)    { return pf_require_live(std::as_const(a), h).x; }
inline double      pf_label_get_y(DrawingArena<LabelRec>& a, Label h)    { return pf_require_live(std::as_const(a), h).y; }
inline std::string pf_label_get_text(DrawingArena<LabelRec>& a, Label h) { return pf_require_live(std::as_const(a), h).text; }

inline void pf_label_set_x(DrawingArena<LabelRec>& a, Label h, int64_t v) { pf_require_live(a, h).x = v; }
inline void pf_label_set_y(DrawingArena<LabelRec>& a, Label h, double v)  { pf_require_live(a, h).y = v; }
inline void pf_label_set_xy(DrawingArena<LabelRec>& a, Label h, int64_t x, double y) {
    LabelRec& r = pf_require_live(a, h); r.x = x; r.y = y;
}
inline void pf_label_set_point(DrawingArena<LabelRec>& a, Label h, ChartPoint p) {
    LabelRec& r = pf_require_live(a, h); r.x = pf_point_x(p, r.xloc); r.y = p.price;
}
inline void pf_label_set_xloc(DrawingArena<LabelRec>& a, Label h, int64_t x, XLoc xloc) {
    LabelRec& r = pf_require_live(a, h); r.x = x; r.xloc = xloc;  // sets x AND xloc
}
inline void pf_label_set_yloc(DrawingArena<LabelRec>& a, Label h, YLoc yloc) {
    pf_require_live(a, h).yloc = yloc;
}
inline void pf_label_set_text(DrawingArena<LabelRec>& a, Label h, std::string text) {
    pf_require_live(a, h).text = std::move(text);
}

inline Label pf_label_copy(DrawingArena<LabelRec>& a, Label h) {
    if (!a.alive(h.id)) return Label{};
    LabelRec r = std::as_const(a).at(h.id);             // deep copy (incl. std::string text)
    return Label{ a.alloc(std::move(r)) };
}
inline void pf_label_delete(DrawingArena<LabelRec>& a, Label h) { a.erase(h.id); }

// ============================ linefill ======================================
inline Linefill pf_linefill_new(DrawingArena<LinefillRec>& a, Line l1, Line l2) {
    // Stores the two Line handle ids inertly (color dropped). linefill.new(na,na,na)
    // is legal -> a LIVE record over na line ids (spec note 1).
    return Linefill{ a.alloc(LinefillRec{ l1.id, l2.id, true }) };
}
inline Line pf_linefill_get_line1(DrawingArena<LinefillRec>& a, Linefill h) {
    return Line{ pf_require_live(std::as_const(a), h).line1 };
}
inline Line pf_linefill_get_line2(DrawingArena<LinefillRec>& a, Linefill h) {
    return Line{ pf_require_live(std::as_const(a), h).line2 };
}
inline void pf_linefill_delete(DrawingArena<LinefillRec>& a, Linefill h) { a.erase(h.id); }

// ============================ visual no-op sink (spec §3.6) ==================
// Evaluates + discards args (so side effects/typechecking still happen), returns void.
// Backs every VISUAL setter/ctor-kwarg: line/box/label set_color/set_style/...
template <class... A> inline void pf_noop(A&&...) {}

} // namespace pineforge
