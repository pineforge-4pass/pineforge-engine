#pragma once
//
// PineForge drawing-objects-as-data runtime.
//
// Spec: docs/drawing-objects-as-data.md §3 ("C++ Runtime").
//
// This is a HEADLESS backtester: drawing objects (line/box/label/linefill) are
// pure DATA the strategy can create, mutate and read back into trading logic.
// There is NO rendering, NO graphics — only geometry. Functions are inline;
// coded errors require the matching PineForge runtime at link time. Codegen emits
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
// back with the script's variables. Unbound arenas retain the vector/shared-
// block representation below. Explicitly configured slot arenas share a
// complete occupied value and stage mutations before publication. Both
// restore logical IDs together with the arena value.
//
#include <array>
#include <atomic>
#include <vector>
#include <deque>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <cmath>
#include <type_traits>
#include <utility>
#include <pineforge/na.hpp>
#include <pineforge/run_failure.hpp>

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

enum class DrawingKind : std::uint8_t { line, box, label, linefill };
enum class DrawingAccessStatus : std::uint8_t { invalid, stale, present };

// Allocation outcomes are diagnostic state.  In particular,
// drawing_slot_bound_exceeded is an internal invariant outcome: it is never
// thrown as a script-visible drawing error.
enum class DrawingAllocationStatus : std::uint8_t {
    ok,
    drawing_slot_bound_exceeded,
    pine_drawing_id_exhausted,
};

struct DrawingAllocationDiagnostic {
    DrawingAllocationStatus status = DrawingAllocationStatus::ok;
    DrawingKind kind = DrawingKind::line;
    std::int32_t configured_bound = 0;
    std::uint64_t required_slots = 0;
    std::uint32_t pending_creation_slots = 0;
};

// A failed creation has no issued ID.  This result is for engine diagnostics;
// it has no implicit handle or integer conversion and is not a script value.
struct DrawingAllocationResult {
    DrawingAllocationDiagnostic diagnostic;
    std::optional<std::int32_t> id;
};

// ---- error type (spec §3.7) ------------------------------------------------
// Derives from std::runtime_error so BacktestEngine::run()'s existing
// `catch (const std::exception& e)` (src/engine_run.cpp) halts the backtest
// exactly like TradingView when a drawing op touches a na handle.
// Invalid script drawing accesses retain the existing typed drawing errors.
struct pine_drawing_error : std::runtime_error {
    explicit pine_drawing_error(const std::string& msg) : std::runtime_error(msg) {}
};

struct pine_drawing_id_exhausted : pine_drawing_error {
    explicit pine_drawing_id_exhausted(const char* object)
        : pine_drawing_error(std::string(object) + " drawing ID domain exhausted") {}
};

namespace detail {

struct DrawingBuiltinAccess;
struct DrawingArenaTestAccess;

template <class Rec> struct drawing_record_traits {
    static constexpr bool builtin = false;
};
template <> struct drawing_record_traits<LineRec> {
    static constexpr bool builtin = true;
    static constexpr DrawingKind kind = DrawingKind::line;
};
template <> struct drawing_record_traits<BoxRec> {
    static constexpr bool builtin = true;
    static constexpr DrawingKind kind = DrawingKind::box;
};
template <> struct drawing_record_traits<LabelRec> {
    static constexpr bool builtin = true;
    static constexpr DrawingKind kind = DrawingKind::label;
};
template <> struct drawing_record_traits<LinefillRec> {
    static constexpr bool builtin = true;
    static constexpr DrawingKind kind = DrawingKind::linefill;
};

template <class Rec>
inline constexpr bool is_builtin_drawing_record_v = drawing_record_traits<Rec>::builtin;

template <class Rec>
constexpr DrawingKind drawing_kind_of() noexcept {
    static_assert(is_builtin_drawing_record_v<Rec>, "name the drawing kind of this record");
    return drawing_record_traits<Rec>::kind;
}

template <class Rec>
inline Rec drawing_missing_value() noexcept {
    if constexpr (std::is_same_v<Rec, LineRec>) {
        return {na<int64_t>(), na<int64_t>(), na<double>(), na<double>(),
                static_cast<XLoc>(-1), false, false, false};
    } else if constexpr (std::is_same_v<Rec, BoxRec>) {
        return {na<int64_t>(), na<int64_t>(), na<double>(), na<double>(),
                static_cast<XLoc>(-1), false};
    } else if constexpr (std::is_same_v<Rec, LabelRec>) {
        return {na<int64_t>(), na<double>(), static_cast<XLoc>(-1),
                static_cast<YLoc>(-1), std::string{}, false};
    } else if constexpr (std::is_same_v<Rec, LinefillRec>) {
        return {-1, -1, false};
    } else {
        static_assert(is_builtin_drawing_record_v<Rec>, "missing value is only for drawing records");
    }
}

// One immutable object per builtin kind across translation units. It is never
// exposed by a mutable accessor and owns no out-of-line text.
template <class Rec>
inline const Rec& drawing_missing_record() noexcept {
    static const Rec missing = drawing_missing_value<Rec>();
    return missing;
}

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

template <class Rec, bool Builtin = is_builtin_drawing_record_v<Rec>>
struct DrawingStaleSink {
    DrawingStaleSink() noexcept = default;
    DrawingStaleSink(const DrawingStaleSink&) noexcept = default;
    DrawingStaleSink& operator=(const DrawingStaleSink&) noexcept = default;
    DrawingStaleSink(DrawingStaleSink&&) noexcept = default;
    DrawingStaleSink& operator=(DrawingStaleSink&&) noexcept = default;
    void reset() const noexcept {}
};

template <class Rec>
struct DrawingStaleSink<Rec, true> {
    mutable Rec value;
    mutable std::atomic_flag resetting = ATOMIC_FLAG_INIT;

    DrawingStaleSink() : value(drawing_missing_value<Rec>()) {}
    DrawingStaleSink(const DrawingStaleSink& other) : value(drawing_missing_value<Rec>()) {
        other.reset();
    }
    DrawingStaleSink& operator=(const DrawingStaleSink& other) {
        reset();
        if (this != &other) other.reset();
        return *this;
    }
    DrawingStaleSink(DrawingStaleSink&& other) noexcept
        : value(drawing_missing_value<Rec>()) {
        other.reset();
    }
    DrawingStaleSink& operator=(DrawingStaleSink&& other) noexcept {
        reset();
        if (this != &other) other.reset();
        return *this;
    }

    void reset() const noexcept {
        // Stable-source const copies may reset the same source sink at once.
        // Caller writes through escaped references must have ended first.
        while (resetting.test_and_set(std::memory_order_acquire)) {}
        if constexpr (std::is_same_v<Rec, LineRec>) {
            value.x1 = na<int64_t>();
            value.x2 = na<int64_t>();
            value.y1 = na<double>();
            value.y2 = na<double>();
            value.xloc = static_cast<XLoc>(-1);
            value.ext_l = false;
            value.ext_r = false;
            value.alive = false;
        } else if constexpr (std::is_same_v<Rec, BoxRec>) {
            value.left = na<int64_t>();
            value.right = na<int64_t>();
            value.top = na<double>();
            value.bottom = na<double>();
            value.xloc = static_cast<XLoc>(-1);
            value.alive = false;
        } else if constexpr (std::is_same_v<Rec, LabelRec>) {
            value.x = na<int64_t>();
            value.y = na<double>();
            value.xloc = static_cast<XLoc>(-1);
            value.yloc = static_cast<YLoc>(-1);
            std::string{}.swap(value.text);
            value.alive = false;
        } else {
            value.line1 = -1;
            value.line2 = -1;
            value.alive = false;
        }
        resetting.clear(std::memory_order_release);
    }
};

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

} // namespace detail

struct DrawingBoundConfig {
    // Zero permits no settled occupants; separately configured pending
    // workspace may still hold a creation. Zero for both terms returns the
    // typed internal bound outcome on the first creation.
    std::int32_t slot_bound = 0;
    // Pending creation demand is transaction workspace accounted separately
    // from the sampled occupied bound.  One default workspace slot follows
    // from this API accepting exactly one creation per call; it is not a
    // measured TradingView allocation peak or a collection threshold.
    std::uint32_t pending_creation_slots = 1;
};

inline DrawingBoundConfig drawing_bound(std::int32_t slot_bound,
                                        std::uint32_t pending_creation_slots = 1) {
    if (slot_bound < 0)
        throw std::invalid_argument("drawing slot bound must be nonnegative");
    return {slot_bound, pending_creation_slots};
}

namespace detail {

inline constexpr std::int32_t kDrawingObservedSlack = 5;
inline constexpr std::int32_t kDrawingLinefillObservedBound = 10;

// The supplied TradingView measurements provide exactly two declared-count
// observations for this default: labels reached 15 at declared 10, and lines
// and boxes reached 10 at declared 5.  The resulting +5 is the measured
// default used here; those two points are the whole evidence, not a universal
// collection law.
template <class Rec>
inline DrawingBoundConfig drawing_bound_from_declared_count(
        std::int32_t declared_count = 50, std::uint32_t pending_creation_slots = 1) {
    static_assert(std::is_same_v<Rec, LineRec> || std::is_same_v<Rec, BoxRec>
                      || std::is_same_v<Rec, LabelRec>,
                  "this default requires a declared count for line, box, or label");
    if (declared_count < 0
        || declared_count > std::numeric_limits<std::int32_t>::max() - kDrawingObservedSlack)
        throw std::invalid_argument("drawing declared count is outside the int32 domain");
    return {static_cast<std::int32_t>(declared_count + kDrawingObservedSlack),
            pending_creation_slots};
}

// The supplied TradingView measurement has no declaration for this kind.  Its
// only qualified sample reached 10 while the line sample used declared 5; the
// measurement is silent about any other relation.  Keep that observed value
// as a caller-settable parameter rather than deriving a new rule.
inline DrawingBoundConfig drawing_bound_from_linefill_sample(
        std::int32_t sampled_bound = kDrawingLinefillObservedBound,
        std::uint32_t pending_creation_slots = 1) {
    return drawing_bound(sampled_bound, pending_creation_slots);
}

template <class Rec>
struct BoundDrawingState {
    struct Slot {
        std::int32_t logical_id = -1;
        std::optional<Rec> value;
    };
    struct DirectoryEntry {
        std::int32_t logical_id;
        std::uint32_t slot;
    };

    static std::size_t storage_slots(DrawingBoundConfig config) {
        if (config.slot_bound < 0)
            throw std::invalid_argument("drawing slot bound must be nonnegative");
        const std::uint64_t total = std::min(
            static_cast<std::uint64_t>(config.slot_bound) + config.pending_creation_slots,
            static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()));
        if (total > std::numeric_limits<std::size_t>::max())
            throw std::length_error("drawing slot storage size overflow");
        return static_cast<std::size_t>(total);
    }

    explicit BoundDrawingState(DrawingBoundConfig config)
        : slots(storage_slots(config)),
          slot_bound(config.slot_bound),
          pending_creation_slots(config.pending_creation_slots) {
        directory.reserve(slots.size());
    }

    std::vector<Slot> slots;
    std::vector<DirectoryEntry> directory; // sorted; contains occupied IDs only
    std::deque<std::int32_t> order;
    std::int64_t next_id = 0;
    std::int32_t slot_bound = 0;
    std::uint32_t pending_creation_slots = 0;

    std::int32_t slot_for(std::int32_t id) const noexcept {
        const auto it = std::lower_bound(
            directory.begin(), directory.end(), id,
            [](const DirectoryEntry& entry, std::int32_t value) {
                return entry.logical_id < value;
            });
        if (it == directory.end() || it->logical_id != id) return -1;
        // A directory hit with no matching occupant is internal corruption,
        // never a stale drawing.  It cannot be produced by public operations.
        if (it->slot >= slots.size()) std::terminate();
        const Slot& candidate = slots[it->slot];
        if (!candidate.value || candidate.logical_id != id) std::terminate();
        return static_cast<std::int32_t>(it->slot);
    }

    std::size_t free_slot() const noexcept {
        for (std::size_t i = 0; i < slots.size(); ++i)
            if (!slots[i].value) return i;
        return slots.size();
    }
};

} // namespace detail

template <class Rec>
inline DrawingBoundConfig drawing_bound_from_declared_count(
        std::int32_t declared_count = 50, std::uint32_t pending_creation_slots = 1) {
    return detail::drawing_bound_from_declared_count<Rec>(
        declared_count, pending_creation_slots);
}

inline DrawingBoundConfig drawing_bound_from_linefill_sample(
        std::int32_t sampled_bound = detail::kDrawingLinefillObservedBound,
        std::uint32_t pending_creation_slots = 1) {
    return detail::drawing_bound_from_linefill_sample(
        sampled_bound, pending_creation_slots);
}

// ---- arena template (spec §3.4) --------------------------------------------
// The default constructor retains the legacy resident COW arena.  A
// DrawingBoundConfig selects the bounded path: logical IDs remain monotonic,
// only occupied IDs are in the sorted directory, and physical slots are
// reclaimed on delete.  The two paths are deliberately separate so generic
// records and unbound callers keep their old direct-reference behavior.
template <class Rec>
class DrawingArena {
    friend struct detail::DrawingBuiltinAccess;
    friend struct detail::DrawingArenaTestAccess;
    detail::DrawingRecordStore<Rec> recs_;
    std::deque<int32_t> order_;
    int cap_;
    std::shared_ptr<detail::BoundDrawingState<Rec>> bound_;
    std::optional<DrawingBoundConfig> bound_config_;
    detail::DrawingStaleSink<Rec> stale_sink_;

    static constexpr std::int64_t kIdExclusive =
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());

    void ensure_bound_unique() {
        if (bound_ && !bound_.unique())
            bound_ = std::make_shared<detail::BoundDrawingState<Rec>>(*bound_);
    }

    const Rec& bound_read(int32_t id) const {
        if (id < 0 || !bound_ || static_cast<std::int64_t>(id) >= bound_->next_id)
            detail::drawing_na_error(detail::drawing_object_name<Rec>(),
                                     "drawing access on na handle");
        const std::int32_t slot = bound_->slot_for(id);
        if (slot < 0) return detail::drawing_missing_record<Rec>();
        return *bound_->slots[static_cast<std::uint32_t>(slot)].value;
    }

    template <class Edit>
    void bound_edit(int32_t id, Edit&& edit) {
        if (id < 0 || !bound_ || static_cast<std::int64_t>(id) >= bound_->next_id)
            detail::drawing_na_error(detail::drawing_object_name<Rec>(),
                                     "drawing access on na handle");
        const std::int32_t slot = bound_->slot_for(id);
        if (slot < 0) return; // stale builtins are typed no-ops
        if (!bound_->slots[static_cast<std::uint32_t>(slot)].value->alive) return;

        auto candidate = std::make_shared<detail::BoundDrawingState<Rec>>(*bound_);
        const std::int32_t candidate_slot = candidate->slot_for(id);
        if (candidate_slot < 0) std::terminate();
        std::forward<Edit>(edit)(
            *candidate->slots[static_cast<std::uint32_t>(candidate_slot)].value);
        stale_sink_.reset();
        bound_.swap(candidate);
    }

public:
    explicit DrawingArena(int cap = 50) : cap_(std::max(1, cap)) {}
    explicit DrawingArena(DrawingBoundConfig config) : cap_(50), bound_config_(config) {
        static_assert(detail::is_builtin_drawing_record_v<Rec>,
                      "bounded arenas are only available for drawing records");
        static_assert(!std::is_same_v<Rec, LinefillRec>,
                      "bounded linefill deletion lifetime is not yet specified");
        if (config.slot_bound < 0)
            throw std::invalid_argument("drawing slot bound must be nonnegative");
        bound_ = std::make_shared<detail::BoundDrawingState<Rec>>(config);
    }

    DrawingArena(const DrawingArena& other)
        : recs_(other.recs_), order_(other.order_), cap_(other.cap_),
          bound_(other.bound_), bound_config_(other.bound_config_),
          stale_sink_() {
        other.stale_sink_.reset();
    }

    DrawingArena& operator=(const DrawingArena& other) {
        if (this != &other) {
            // All fallible work precedes publication and either sink reset.
            detail::DrawingRecordStore<Rec> records(other.recs_);
            auto order = other.order_;
            auto state = other.bound_;
            recs_ = std::move(records);
            order_.swap(order);
            cap_ = other.cap_;
            bound_.swap(state);
            bound_config_ = other.bound_config_;
            other.stale_sink_.reset();
        }
        stale_sink_.reset();
        return *this;
    }

    // A moved-from arena is empty and keeps its legacy cap.  Bound state is
    // transferred as one value; its source is left with no issued IDs.
    DrawingArena(DrawingArena&& other)
        : recs_(std::move(other.recs_)), order_(std::move(other.order_)),
          cap_(other.cap_), bound_(std::move(other.bound_)),
          bound_config_(other.bound_config_),
          stale_sink_(std::move(other.stale_sink_)) {
        other.order_.clear();
    }

    DrawingArena& operator=(DrawingArena&& other) noexcept {
        if (this != &other) {
            recs_ = std::move(other.recs_);
            order_ = std::move(other.order_);
            bound_ = std::move(other.bound_);
            bound_config_ = other.bound_config_;
            stale_sink_ = std::move(other.stale_sink_);
            other.order_.clear();
            cap_ = other.cap_;
        }
        stale_sink_.reset();
        return *this;
    }

    // Creation for explicitly configured slot arenas.  An invariant outcome
    // contains no ID and does not modify diagnostic or logical arena state.
    // The generated caller must consume this result before publishing a
    // script handle.  Genuine allocation failures retain their C++ exception.
    [[nodiscard]] DrawingAllocationResult try_alloc(Rec r) {
        static_assert(detail::is_builtin_drawing_record_v<Rec>,
                      "slot allocation requires a builtin drawing record");
        if (!bound_config_)
            throw std::logic_error("typed drawing allocation requires a slot arena");
        DrawingAllocationDiagnostic diagnostic{};
        diagnostic.kind = detail::drawing_kind_of<Rec>();
        diagnostic.configured_bound = bound_config_->slot_bound;
        diagnostic.pending_creation_slots = bound_config_->pending_creation_slots;
        const std::int64_t next_id = bound_ ? bound_->next_id : 0;
        if (next_id >= kIdExclusive) {
            diagnostic.status = DrawingAllocationStatus::pine_drawing_id_exhausted;
            return {diagnostic, std::nullopt};
        }
        const std::uint64_t required = (bound_ ? bound_->directory.size() : 0u) + 1u;
        diagnostic.required_slots = required;
        const std::uint64_t available =
            static_cast<std::uint64_t>(bound_config_->slot_bound)
            + bound_config_->pending_creation_slots;
        if (required > available) {
            diagnostic.status = DrawingAllocationStatus::drawing_slot_bound_exceeded;
            return {diagnostic, std::nullopt};
        }
        auto candidate = bound_
            ? std::make_shared<detail::BoundDrawingState<Rec>>(*bound_)
            : std::make_shared<detail::BoundDrawingState<Rec>>(*bound_config_);
        const std::size_t slot = candidate->free_slot();
        if (slot >= candidate->slots.size()) std::terminate(); // corrupt occupied count
        const std::int32_t id = static_cast<std::int32_t>(next_id);
        r.alive = true;
        candidate->slots[slot].logical_id = id;
        candidate->slots[slot].value.emplace(std::move(r));
        candidate->directory.push_back({id, static_cast<std::uint32_t>(slot)});
        candidate->order.push_back(id);
        candidate->next_id = next_id + 1;
        stale_sink_.reset();
        bound_.swap(candidate);
        return {diagnostic, id};
    }

    int32_t alloc(Rec r) {
        // The legacy integer-only entry has no status channel.  Binding the
        // new path requires its typed creation lowering first; do not convert
        // a slot invariant into an NA handle or a script drawing error.
        if (bound_config_)
            throw std::logic_error("slot drawing creation requires try_alloc");
        if (static_cast<std::int64_t>(recs_.size()) >= kIdExclusive) {
            if constexpr (detail::is_builtin_drawing_record_v<Rec>)
                throw pine_drawing_id_exhausted(detail::drawing_object_name<Rec>());
            else
                throw pine_drawing_id_exhausted("generic");
        }

        while ((int)order_.size() >= cap_) {        // exact-cap FIFO eviction (oldest first)
            // Tombstone before leaving order_: the write may copy a shared block
            // and throw, which must leave the record live and in order_.
            recs_.write(static_cast<std::uint32_t>(order_.front())).alive = false;
            order_.pop_front();
        }
        int32_t id = recs_.size();
        r.alive = true;
        recs_.push_back(std::move(r));
        order_.push_back(id);
        return id;
    }

    void erase(int32_t id) {
        if (bound_) {
            if (id < 0 || static_cast<std::int64_t>(id) >= bound_->next_id) return;
            const std::int32_t slot = bound_->slot_for(id);
            if (slot < 0) return; // stale delete is a no-op
            if (!bound_->slots[static_cast<std::uint32_t>(slot)].value->alive) return;

            auto candidate = std::make_shared<detail::BoundDrawingState<Rec>>(*bound_);
            const std::int32_t candidate_slot = candidate->slot_for(id);
            if (candidate_slot < 0) std::terminate();
            const auto dir = std::lower_bound(
                candidate->directory.begin(), candidate->directory.end(), id,
                [](const auto& entry, std::int32_t value) {
                    return entry.logical_id < value;
                });
            if (dir == candidate->directory.end() || dir->logical_id != id) std::terminate();
            candidate->directory.erase(dir);
            const auto it = std::find(candidate->order.begin(), candidate->order.end(), id);
            if (it != candidate->order.end()) candidate->order.erase(it);
            candidate->slots[static_cast<std::uint32_t>(candidate_slot)].logical_id = -1;
            candidate->slots[static_cast<std::uint32_t>(candidate_slot)].value.reset();
            stale_sink_.reset();
            bound_.swap(candidate);
            return;
        }

        if (!alive(id)) return;
        recs_.write(static_cast<std::uint32_t>(id)).alive = false;
        const auto it = std::find(order_.begin(), order_.end(), id);
        if (it != order_.end()) order_.erase(it);
    }

    bool alive(int32_t id) const {
        if (bound_) {
            const std::int32_t slot = (id >= 0) ? bound_->slot_for(id) : -1;
            return slot >= 0
                && bound_->slots[static_cast<std::uint32_t>(slot)].value->alive;
        }
        return id >= 0 && id < recs_.size() && recs_.read(static_cast<std::uint32_t>(id)).alive;
    }

    // In the unbound path this is the issued record count.  In the bound path
    // it is the next logical ID, so a reclaimed slot never changes old IDs.
    int32_t size() const {
        if (bound_) return static_cast<int32_t>(bound_->next_id);
        return recs_.size();
    }

    DrawingAccessStatus access_status(int32_t id) const noexcept {
        if (id < 0) return DrawingAccessStatus::invalid;
        if (bound_) {
            if (static_cast<std::int64_t>(id) >= bound_->next_id)
                return DrawingAccessStatus::invalid;
            return bound_->slot_for(id) < 0 ? DrawingAccessStatus::stale
                                            : DrawingAccessStatus::present;
        }
        return id >= recs_.size() ? DrawingAccessStatus::invalid
                                   : DrawingAccessStatus::present;
    }

    bool is_bound() const noexcept { return bound_config_.has_value(); }
    std::int32_t slot_bound() const noexcept {
        return bound_config_ ? bound_config_->slot_bound : 0;
    }
    std::uint32_t pending_creation_slots() const noexcept {
        return bound_config_ ? bound_config_->pending_creation_slots : 0;
    }
    std::size_t occupied_slots() const noexcept {
        return bound_ ? bound_->directory.size() : order_.size();
    }

    // Bound direct borrows classify stale IDs before returning the immutable
    // NA record or this arena object's reset sink.  Unbound raw access keeps
    // its historical unchecked reference representation.
    Rec& at(int32_t id) {
        if constexpr (detail::is_builtin_drawing_record_v<Rec>) {
            if (bound_config_) {
                if (id < 0 || !bound_ || static_cast<std::int64_t>(id) >= bound_->next_id)
                    detail::drawing_na_error(detail::drawing_object_name<Rec>(),
                                             "drawing access on na handle");
                if (bound_->slot_for(id) < 0) {
                    stale_sink_.reset();
                    return stale_sink_.value;
                }
                ensure_bound_unique();
                stale_sink_.reset();
                const std::int32_t slot = bound_->slot_for(id);
                return *bound_->slots[static_cast<std::uint32_t>(slot)].value;
            }
        }
        return recs_.write(static_cast<std::uint32_t>(id));
    }

    const Rec& at(int32_t id) const {
        if constexpr (detail::is_builtin_drawing_record_v<Rec>) {
            if (bound_config_) return bound_read(id);
        }
        return recs_.read(static_cast<std::uint32_t>(id));
    }

    const std::deque<int32_t>& order() const {
        return bound_ ? bound_->order : order_;
    }
};

// ---- shared helpers --------------------------------------------------------
// Direct C++ borrow compatibility: checked invalid handles throw; an unbound
// resident dead record is returned as-is. Builtins use separate value access.
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

namespace detail {

// Builtins use the bound directory directly and never acquire a public
// mutable borrow or write a discard sink. The unbound missing policy uses
// alive for all removals; it is not evidence about TradingView's getters
// after automatic collection.
struct DrawingBuiltinAccess {
    template <class Rec, class Handle>
    static bool missing(const DrawingArena<Rec>& a, Handle h) noexcept {
        if (a.bound_config_) {
            if (!a.bound_) return false; // no issued IDs in a moved-from arena
            const auto slot = a.bound_->slot_for(h.id);
            return slot < 0
                || !a.bound_->slots[static_cast<std::uint32_t>(slot)].value->alive;
        }
        if constexpr (std::is_same_v<Rec, LinefillRec>) return false;
        else return !a.alive(h.id);
    }

    template <class Rec, class Handle>
    static const Rec& read(const DrawingArena<Rec>& a, Handle h) {
        if (h.id < 0 || h.id >= a.size())
            drawing_na_error(drawing_object_name<Rec>(), "drawing access on na handle");
        if (missing(a, h)) return drawing_missing_record<Rec>();
        if (a.bound_config_) return a.bound_read(h.id);
        const Rec& r = a.recs_.read(static_cast<std::uint32_t>(h.id));
        return r;
    }

    template <class Rec, class Handle, class Edit>
    static void edit(DrawingArena<Rec>& a, Handle h, Edit&& apply) {
        if (h.id < 0 || h.id >= a.size())
            drawing_na_error(drawing_object_name<Rec>(), "drawing access on na handle");
        if (missing(a, h)) return;
        if (a.bound_config_) {
            a.bound_edit(h.id, std::forward<Edit>(apply));
            return;
        }
        apply(a.recs_.write(static_cast<std::uint32_t>(h.id)));
    }
};

} // namespace detail

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

inline int64_t pf_line_get_x1(DrawingArena<LineRec>& a, Line h) { return detail::DrawingBuiltinAccess::read(a, h).x1; }
inline int64_t pf_line_get_x2(DrawingArena<LineRec>& a, Line h) { return detail::DrawingBuiltinAccess::read(a, h).x2; }
inline double  pf_line_get_y1(DrawingArena<LineRec>& a, Line h) { return detail::DrawingBuiltinAccess::read(a, h).y1; }
inline double  pf_line_get_y2(DrawingArena<LineRec>& a, Line h) { return detail::DrawingBuiltinAccess::read(a, h).y2; }

inline void pf_line_set_x1(DrawingArena<LineRec>& a, Line h, int64_t v) { detail::DrawingBuiltinAccess::edit(a, h, [=](LineRec& r) { r.x1 = v; }); }
inline void pf_line_set_x2(DrawingArena<LineRec>& a, Line h, int64_t v) { detail::DrawingBuiltinAccess::edit(a, h, [=](LineRec& r) { r.x2 = v; }); }
inline void pf_line_set_y1(DrawingArena<LineRec>& a, Line h, double v)  { detail::DrawingBuiltinAccess::edit(a, h, [=](LineRec& r) { r.y1 = v; }); }
inline void pf_line_set_y2(DrawingArena<LineRec>& a, Line h, double v)  { detail::DrawingBuiltinAccess::edit(a, h, [=](LineRec& r) { r.y2 = v; }); }
inline void pf_line_set_xy1(DrawingArena<LineRec>& a, Line h, int64_t x, double y) {
    detail::DrawingBuiltinAccess::edit(a, h, [=](LineRec& r) { r.x1 = x; r.y1 = y; });
}
inline void pf_line_set_xy2(DrawingArena<LineRec>& a, Line h, int64_t x, double y) {
    detail::DrawingBuiltinAccess::edit(a, h, [=](LineRec& r) { r.x2 = x; r.y2 = y; });
}
inline void pf_line_set_first_point(DrawingArena<LineRec>& a, Line h, ChartPoint p) {
    detail::DrawingBuiltinAccess::edit(a, h, [=](LineRec& r) { r.x1 = pf_point_x(p, r.xloc); r.y1 = p.price; });
}
inline void pf_line_set_second_point(DrawingArena<LineRec>& a, Line h, ChartPoint p) {
    detail::DrawingBuiltinAccess::edit(a, h, [=](LineRec& r) { r.x2 = pf_point_x(p, r.xloc); r.y2 = p.price; });
}
inline void pf_line_set_xloc(DrawingArena<LineRec>& a, Line h, int64_t x1, int64_t x2, XLoc xloc) {
    // REINTERPRET: stores new numbers + flag; does NOT convert coord spaces (TV-faithful, spec §3.9).
    detail::DrawingBuiltinAccess::edit(a, h, [=](LineRec& r) { r.x1 = x1; r.x2 = x2; r.xloc = xloc; });
}

// real computation: infinite line, ignores extend; valid for xloc.bar_index (spec §3.6).
inline double pf_line_get_price(DrawingArena<LineRec>& a, Line h, int64_t x) {
    const LineRec& r = detail::DrawingBuiltinAccess::read(a, h);
    if (detail::DrawingBuiltinAccess::missing(a, h)) return na<double>();
    if (r.xloc == XLoc::bar_time)                 // TV errors on bar_time lines
        detail::drawing_argument_error("line.get_price", "xloc", "requires_xloc_bar_index",
                                       "line.get_price requires xloc.bar_index");
    if (r.x1 == r.x2) return na<double>();              // degenerate vertical -> na (matches TV)
    return r.y1 + (r.y2 - r.y1) / (double)(r.x2 - r.x1) * (double)(x - r.x1); // infinite line, ignores extend
}

inline Line pf_line_copy(DrawingArena<LineRec>& a, Line h) {
    if (!a.alive(h.id)) return Line{};
    LineRec r = detail::DrawingBuiltinAccess::read(a, h);
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

inline int64_t pf_box_get_left  (DrawingArena<BoxRec>& a, Box h) { return detail::DrawingBuiltinAccess::read(a, h).left; }
inline int64_t pf_box_get_right (DrawingArena<BoxRec>& a, Box h) { return detail::DrawingBuiltinAccess::read(a, h).right; }
inline double  pf_box_get_top   (DrawingArena<BoxRec>& a, Box h) { return detail::DrawingBuiltinAccess::read(a, h).top; }
inline double  pf_box_get_bottom(DrawingArena<BoxRec>& a, Box h) { return detail::DrawingBuiltinAccess::read(a, h).bottom; }

inline void pf_box_set_left  (DrawingArena<BoxRec>& a, Box h, int64_t v) { detail::DrawingBuiltinAccess::edit(a, h, [=](BoxRec& r) { r.left = v; }); }
inline void pf_box_set_right (DrawingArena<BoxRec>& a, Box h, int64_t v) { detail::DrawingBuiltinAccess::edit(a, h, [=](BoxRec& r) { r.right = v; }); }
inline void pf_box_set_top   (DrawingArena<BoxRec>& a, Box h, double v)  { detail::DrawingBuiltinAccess::edit(a, h, [=](BoxRec& r) { r.top = v; }); }
inline void pf_box_set_bottom(DrawingArena<BoxRec>& a, Box h, double v)  { detail::DrawingBuiltinAccess::edit(a, h, [=](BoxRec& r) { r.bottom = v; }); }
inline void pf_box_set_lefttop(DrawingArena<BoxRec>& a, Box h, int64_t left, double top) {
    detail::DrawingBuiltinAccess::edit(a, h, [=](BoxRec& r) { r.left = left; r.top = top; });
}
inline void pf_box_set_rightbottom(DrawingArena<BoxRec>& a, Box h, int64_t right, double bottom) {
    detail::DrawingBuiltinAccess::edit(a, h, [=](BoxRec& r) { r.right = right; r.bottom = bottom; });
}
inline void pf_box_set_top_left_point(DrawingArena<BoxRec>& a, Box h, ChartPoint p) {
    detail::DrawingBuiltinAccess::edit(a, h, [=](BoxRec& r) { r.left = pf_point_x(p, r.xloc); r.top = p.price; });
}
inline void pf_box_set_bottom_right_point(DrawingArena<BoxRec>& a, Box h, ChartPoint p) {
    detail::DrawingBuiltinAccess::edit(a, h, [=](BoxRec& r) { r.right = pf_point_x(p, r.xloc); r.bottom = p.price; });
}
inline void pf_box_set_xloc(DrawingArena<BoxRec>& a, Box h, int64_t left, int64_t right, XLoc xloc) {
    detail::DrawingBuiltinAccess::edit(a, h, [=](BoxRec& r) { r.left = left; r.right = right; r.xloc = xloc; });
}

inline Box pf_box_copy(DrawingArena<BoxRec>& a, Box h) {
    if (!a.alive(h.id)) return Box{};
    BoxRec r = detail::DrawingBuiltinAccess::read(a, h);
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

inline int64_t     pf_label_get_x(DrawingArena<LabelRec>& a, Label h)    { return detail::DrawingBuiltinAccess::read(a, h).x; }
inline double      pf_label_get_y(DrawingArena<LabelRec>& a, Label h)    { return detail::DrawingBuiltinAccess::read(a, h).y; }
inline std::string pf_label_get_text(DrawingArena<LabelRec>& a, Label h) { return detail::DrawingBuiltinAccess::read(a, h).text; }

inline void pf_label_set_x(DrawingArena<LabelRec>& a, Label h, int64_t v) { detail::DrawingBuiltinAccess::edit(a, h, [=](LabelRec& r) { r.x = v; }); }
inline void pf_label_set_y(DrawingArena<LabelRec>& a, Label h, double v)  { detail::DrawingBuiltinAccess::edit(a, h, [=](LabelRec& r) { r.y = v; }); }
inline void pf_label_set_xy(DrawingArena<LabelRec>& a, Label h, int64_t x, double y) {
    detail::DrawingBuiltinAccess::edit(a, h, [=](LabelRec& r) { r.x = x; r.y = y; });
}
inline void pf_label_set_point(DrawingArena<LabelRec>& a, Label h, ChartPoint p) {
    detail::DrawingBuiltinAccess::edit(a, h, [=](LabelRec& r) { r.x = pf_point_x(p, r.xloc); r.y = p.price; });
}
inline void pf_label_set_xloc(DrawingArena<LabelRec>& a, Label h, int64_t x, XLoc xloc) {
    detail::DrawingBuiltinAccess::edit(a, h, [=](LabelRec& r) { r.x = x; r.xloc = xloc; });
}
inline void pf_label_set_yloc(DrawingArena<LabelRec>& a, Label h, YLoc yloc) {
    detail::DrawingBuiltinAccess::edit(a, h, [=](LabelRec& r) { r.yloc = yloc; });
}
inline void pf_label_set_text(DrawingArena<LabelRec>& a, Label h, std::string text) {
    detail::DrawingBuiltinAccess::edit(a, h, [&](LabelRec& r) { r.text = std::move(text); });
}

inline Label pf_label_copy(DrawingArena<LabelRec>& a, Label h) {
    if (!a.alive(h.id)) return Label{};
    LabelRec r = detail::DrawingBuiltinAccess::read(a, h);
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
    return Line{ detail::DrawingBuiltinAccess::read(a, h).line1 };
}
inline Line pf_linefill_get_line2(DrawingArena<LinefillRec>& a, Linefill h) {
    return Line{ detail::DrawingBuiltinAccess::read(a, h).line2 };
}
inline void pf_linefill_delete(DrawingArena<LinefillRec>& a, Linefill h) { a.erase(h.id); }

// ============================ visual no-op sink (spec §3.6) ==================
// Evaluates + discards args (so side effects/typechecking still happen), returns void.
// Backs every VISUAL setter/ctor-kwarg: line/box/label set_color/set_style/...
template <class... A> inline void pf_noop(A&&...) {}

} // namespace pineforge
