// CR-150 O1C. Predicted first red on 6cfcff0696: the frozen closure has no
// pineforge/detail/scratch_store.hpp (fatal error: file not found). The DRAFT v0
// declarations (e43d7bcc7) lack stats() and the geometry floor constants, so
// against them the first red is the compile error at the first use of stats().
// Against the stage-one headers (5134128e9), which declare ScratchStore and do
// not define it, the body compiles and the first red is the link. The complete
// body must be compiled there on the lab; no fail-before or passing execution
// is claimed by this edit-only lane.
//
// Every expected value below comes from an independent oracle (a sorted model
// with its own comparator and its own FNV-1a fold), never from the store, and
// every fixture first checks that it reaches the population it claims to test
// (page traffic, comparator calls, fault hits) so none can pass by being idle.
#include "global_allocation_replacement.hpp"
#include <pineforge/detail/scratch_store.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace pineforge::detail;
using Text = std::string;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

const char* name_of(ScratchStatus status) {
    switch (status) {
    case ScratchStatus::ok: return "ok";
    case ScratchStatus::end: return "end";
    case ScratchStatus::cancelled: return "cancelled";
    case ScratchStatus::not_found: return "not_found";
    case ScratchStatus::programming_error: return "programming_error";
    case ScratchStatus::io_error: return "io_error";
    case ScratchStatus::corrupt: return "corrupt";
    }
    return "unknown";
}
void require_status(ScratchStatus got, ScratchStatus want, const std::string& message) {
    if (got == want) return;
    throw std::runtime_error(message + ": got " + name_of(got) + ", want " + name_of(want));
}
void require_status(ScratchStatus got, ScratchStatus want, const char* message) {
    require_status(got, want, std::string(message));
}

// ---- the contract's static shape ------------------------------------------

static_assert(std::is_base_of<ScratchReadback, ScratchStore>::value
              && std::is_final<ScratchStore>::value, "the store is a final ScratchReadback");
static_assert(!std::is_default_constructible<ScratchStore>::value
              && !std::is_copy_constructible<ScratchStore>::value
              && !std::is_move_constructible<ScratchStore>::value
              && !std::is_copy_assignable<ScratchStore>::value
              && !std::is_move_assignable<ScratchStore>::value,
              "one exclusive owner per backing");
static_assert(noexcept(ScratchStore(std::declval<ScratchStoreConfig>(), std::declval<ScratchBacking*>()))
              && std::is_nothrow_destructible<ScratchStore>::value, "construction never throws");
static_assert(noexcept(std::declval<ScratchStore&>().begin())
              && noexcept(std::declval<ScratchStore&>().put(std::declval<const ScratchByteSource&>(),
                                                            std::declval<const ScratchByteSource&>(),
                                                            std::declval<ScratchHandle&>()))
              && noexcept(std::declval<ScratchStore&>().erase(ScratchHandle{}))
              && noexcept(std::declval<ScratchStore&>().commit(ScratchDurability::buffered))
              && noexcept(std::declval<ScratchStore&>().rollback()), "no mutation throws");
static_assert(noexcept(std::declval<const ScratchStore&>().point(ScratchHandle{}, std::declval<ScratchRecordInfo&>()))
              && noexcept(std::declval<const ScratchStore&>().read(ScratchHandle{}, ScratchField::key, 0,
                                                                  ScratchBuffer{}))
              && noexcept(std::declval<const ScratchStore&>().visit(ScratchHandle{}, ScratchBuffer{},
                                                                   ScratchChunkVisitor{}, nullptr))
              && noexcept(std::declval<const ScratchStore&>().for_each(ScratchBuffer{}, ScratchRecordVisitor{},
                                                                      nullptr))
              && noexcept(std::declval<const ScratchStore&>().first(std::declval<ScratchHandle&>()))
              && noexcept(std::declval<const ScratchStore&>().next(ScratchHandle{}, std::declval<ScratchHandle&>()))
              && noexcept(std::declval<const ScratchStore&>().lower_bound(std::declval<const ScratchByteSource&>(),
                                                                         std::declval<ScratchHandle&>()))
              && noexcept(std::declval<const ScratchStore&>().ordered_digest(ScratchBuffer{},
                                                                            std::declval<std::uint64_t&>()))
              && noexcept(std::declval<const ScratchStore&>().state_digest(ScratchBuffer{},
                                                                          std::declval<std::uint64_t&>()))
              && noexcept(std::declval<const ScratchStore&>().stats())
              && noexcept(std::declval<const ScratchStore&>().status()), "no read throws");
static_assert(std::is_same<decltype(std::declval<const ScratchStore&>().stats()), ScratchStoreStats>::value,
              "stats are a plain value");
static_assert(scratch_min_page_count == 2 && scratch_min_page_size == 512, "geometry floor");

// ---- deterministic data ---------------------------------------------------

std::uint64_t mix(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

struct Rng {
    explicit Rng(std::uint64_t seed) : state(seed) {}
    std::uint64_t next() { return mix(state++); }
    std::uint64_t below(std::uint64_t bound) { return next() % bound; }
    std::uint64_t state;
};

Text key_u64(std::uint64_t value) {
    Text key(8, '\0');
    for (int i = 0; i < 8; ++i) key[static_cast<std::size_t>(i)] = static_cast<char>((value >> (8 * i)) & 0xFF);
    return key;
}

std::uint64_t u64_of(const Text& key) {
    std::uint64_t value = 0;
    for (std::size_t i = std::min<std::size_t>(key.size(), 8); i-- > 0;)
        value = (value << 8) | static_cast<unsigned char>(key[i]);
    return value;
}

void store_le64(std::byte* out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) out[i] = static_cast<std::byte>((value >> (8 * i)) & 0xFF);
}

void fill_bytes(std::byte* out, std::size_t length, std::uint64_t seed) {
    for (std::size_t i = 0; i < length; ++i)
        out[i] = static_cast<std::byte>((mix(seed + i / 8) >> (8 * (i % 8))) & 0xFF);
}

Text make_value(std::uint64_t id, std::uint64_t version, std::size_t length) {
    Text value(length, '\0');
    for (std::size_t i = 0; i < length; ++i) {
        const std::uint64_t word = mix(id * 0x100000001B3ull + version * 0x9E3779B1ull + i / 8);
        value[i] = static_cast<char>((word >> (8 * (i % 8))) & 0xFF);
    }
    return value;
}

ScratchBytes bytes_of(const Text& text) {
    return ScratchBytes{reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

// A value that exists only as a formula: it can be read but never held as one
// contiguous buffer, which is what a record larger than the cache looks like.
class PatternSource final : public ScratchByteSource {
public:
    PatternSource(std::uint64_t size, std::uint64_t seed) : size_(size), seed_(seed) {}
    std::uint64_t size() const noexcept override { return size_; }
    ScratchStatus read(std::uint64_t offset, ScratchBuffer destination) const noexcept override {
        if (offset > size_ || destination.size > size_ - offset
            || (destination.size != 0 && destination.data == nullptr))
            return ScratchStatus::programming_error;
        ++reads;
        if (destination.size > max_request) max_request = destination.size;
        for (std::size_t i = 0; i < destination.size; ++i) destination.data[i] = byte_at(seed_, offset + i);
        return ScratchStatus::ok;
    }
    static std::byte byte_at(std::uint64_t seed, std::uint64_t index) {
        return static_cast<std::byte>((mix(seed ^ (index >> 3)) >> ((index & 7) * 8)) & 0xFF);
    }
    mutable std::uint64_t reads = 0;
    mutable std::size_t max_request = 0;

private:
    std::uint64_t size_;
    std::uint64_t seed_;
};

Text pattern_text(std::uint64_t size, std::uint64_t seed) {
    Text text(static_cast<std::size_t>(size), '\0');
    for (std::size_t i = 0; i < text.size(); ++i) text[i] = static_cast<char>(PatternSource::byte_at(seed, i));
    return text;
}

// ---- the independent FNV-1a-64 oracle -------------------------------------

struct Fnv {
    std::uint64_t value = 14695981039346656037ull;
    void byte(unsigned char b) {
        value ^= b;
        value *= 1099511628211ull;
    }
    void le64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) byte(static_cast<unsigned char>((v >> (8 * i)) & 0xFF));
    }
    void text(const Text& s) {
        for (const char c : s) byte(static_cast<unsigned char>(c));
    }
    void raw(const std::byte* data, std::size_t length) {
        for (std::size_t i = 0; i < length; ++i) byte(std::to_integer<unsigned char>(data[i]));
    }
};

void oracle_matches_published_vectors() {
    Fnv empty;
    require(empty.value == 0xcbf29ce484222325ull, "FNV-1a-64 offset basis");
    Fnv a;
    a.byte('a');
    require(a.value == 0xaf63dc4c8601ec8cull, "FNV-1a-64 of \"a\" is the published vector");
    Fnv little;
    little.le64(0x0807060504030201ull);
    Fnv spelled;
    for (unsigned char b = 1; b <= 8; ++b) spelled.byte(b);
    require(little.value == spelled.value, "LE64 emits the least significant byte first");
}

// ---- comparators the caller owns ------------------------------------------

struct OrderProbe {
    std::uint64_t calls = 0;
    std::uint64_t fail_from_call = 0;  // 0 never; else calls >= this return io_error
    std::uint64_t max_end = 0;         // largest offset + length any comparison read
    ScratchStore* victim = nullptr;    // when set, the first comparison attacks it
    bool attempted = false;
    std::array<ScratchStatus, 5> attempts{};
};

void try_mutations(OrderProbe& probe) noexcept {
    static const std::array<std::byte, 8> raw{};
    ScratchSpanSource source(ScratchBytes{raw.data(), raw.size()});
    ScratchHandle handle = 0;
    probe.attempts[0] = probe.victim->begin();
    probe.attempts[1] = probe.victim->put(source, source, handle);
    probe.attempts[2] = probe.victim->erase(1);
    probe.attempts[3] = probe.victim->commit(ScratchDurability::buffered);
    probe.attempts[4] = probe.victim->rollback();
}

ScratchStatus enter_compare(void* context) noexcept {
    auto* probe = static_cast<OrderProbe*>(context);
    if (probe == nullptr) return ScratchStatus::ok;
    ++probe->calls;
    if (probe->victim != nullptr && !probe->attempted) {
        probe->attempted = true;
        try_mutations(*probe);
    }
    if (probe->fail_from_call != 0 && probe->calls >= probe->fail_from_call) return ScratchStatus::io_error;
    return ScratchStatus::ok;
}

ScratchStatus load_u64(const ScratchByteSource& source, std::uint64_t& out) noexcept {
    if (source.size() != 8) return ScratchStatus::programming_error;
    std::byte raw[8];
    const ScratchStatus status = source.read(0, ScratchBuffer{raw, 8});
    if (status != ScratchStatus::ok) return status;
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = (value << 8) | std::to_integer<std::uint64_t>(raw[i]);
    out = value;
    return ScratchStatus::ok;
}

ScratchStatus compare_numeric(void* context, const ScratchByteSource& a, const ScratchByteSource& b,
                              int& order) noexcept {
    const ScratchStatus gate = enter_compare(context);
    if (gate != ScratchStatus::ok) return gate;
    std::uint64_t x = 0, y = 0;
    ScratchStatus status = load_u64(a, x);
    if (status != ScratchStatus::ok) return status;
    status = load_u64(b, y);
    if (status != ScratchStatus::ok) return status;
    order = x < y ? -1 : (x > y ? 1 : 0);
    return ScratchStatus::ok;
}

ScratchStatus compare_numeric_desc(void* context, const ScratchByteSource& a, const ScratchByteSource& b,
                                   int& order) noexcept {
    const ScratchStatus status = compare_numeric(context, a, b, order);
    if (status == ScratchStatus::ok) order = -order;
    return status;
}

ScratchStatus compare_lex(void* context, const ScratchByteSource& a, const ScratchByteSource& b,
                          int& order) noexcept {
    const ScratchStatus gate = enter_compare(context);
    if (gate != ScratchStatus::ok) return gate;
    auto* probe = static_cast<OrderProbe*>(context);
    const std::uint64_t na = a.size(), nb = b.size();
    const std::uint64_t common = na < nb ? na : nb;
    std::byte left[64], right[64];
    for (std::uint64_t at = 0; at < common;) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(64, common - at));
        ScratchStatus status = a.read(at, ScratchBuffer{left, n});
        if (status != ScratchStatus::ok) return status;
        status = b.read(at, ScratchBuffer{right, n});
        if (status != ScratchStatus::ok) return status;
        if (probe != nullptr && at + n > probe->max_end) probe->max_end = at + n;
        const int c = std::memcmp(left, right, n);
        if (c != 0) {
            order = c < 0 ? -1 : 1;
            return ScratchStatus::ok;
        }
        at += n;
    }
    order = na < nb ? -1 : (na > nb ? 1 : 0);
    return ScratchStatus::ok;
}

int fold_char(std::byte b) {
    const int c = std::to_integer<int>(b);
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

int fold_order(const std::byte* x, std::size_t nx, const std::byte* y, std::size_t ny) {
    const std::size_t common = std::min<std::size_t>(nx, ny);
    for (std::size_t i = 0; i < common; ++i) {
        const int cx = fold_char(x[i]), cy = fold_char(y[i]);
        if (cx != cy) return cx < cy ? -1 : 1;
    }
    return nx < ny ? -1 : (nx > ny ? 1 : 0);
}

ScratchStatus compare_fold(void* context, const ScratchByteSource& a, const ScratchByteSource& b,
                           int& order) noexcept {
    const ScratchStatus gate = enter_compare(context);
    if (gate != ScratchStatus::ok) return gate;
    if (a.size() > 64 || b.size() > 64) return ScratchStatus::programming_error;
    std::byte left[64], right[64];
    const std::size_t na = static_cast<std::size_t>(a.size()), nb = static_cast<std::size_t>(b.size());
    ScratchStatus status = a.read(0, ScratchBuffer{left, na});
    if (status != ScratchStatus::ok) return status;
    status = b.read(0, ScratchBuffer{right, nb});
    if (status != ScratchStatus::ok) return status;
    order = fold_order(left, na, right, nb);
    return ScratchStatus::ok;
}

ScratchKeyOrder order_numeric(OrderProbe* probe = nullptr) { return ScratchKeyOrder{1, probe, &compare_numeric}; }
ScratchKeyOrder order_lex(OrderProbe* probe = nullptr) { return ScratchKeyOrder{2, probe, &compare_lex}; }
ScratchKeyOrder order_fold(OrderProbe* probe = nullptr) { return ScratchKeyOrder{4, probe, &compare_fold}; }
ScratchKeyOrder order_numeric_desc(OrderProbe* probe = nullptr) {
    return ScratchKeyOrder{5, probe, &compare_numeric_desc};
}

int cmp_numeric_text(const Text& a, const Text& b) {
    const std::uint64_t x = u64_of(a), y = u64_of(b);
    return x < y ? -1 : (x > y ? 1 : 0);
}
int cmp_numeric_desc_text(const Text& a, const Text& b) { return -cmp_numeric_text(a, b); }
int cmp_lex_text(const Text& a, const Text& b) {
    const std::size_t common = std::min<std::size_t>(a.size(), b.size());
    const int c = common != 0 ? std::memcmp(a.data(), b.data(), common) : 0;
    if (c != 0) return c < 0 ? -1 : 1;
    return a.size() < b.size() ? -1 : (a.size() > b.size() ? 1 : 0);
}
int cmp_fold_text(const Text& a, const Text& b) {
    return fold_order(reinterpret_cast<const std::byte*>(a.data()), a.size(),
                      reinterpret_cast<const std::byte*>(b.data()), b.size());
}

// ---- the oracle: a sorted model of what the store must hold ----------------

struct Row {
    Text key;
    Text value;
    ScratchHandle handle = 0;
};

class Model {
public:
    explicit Model(int (*compare)(const Text&, const Text&)) : compare_(compare) {}

    std::size_t lower(const Text& key) const {
        std::size_t lo = 0, hi = rows.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (compare_(rows[mid].key, key) < 0) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }
    bool find(const Text& key, std::size_t& at) const {
        at = lower(key);
        return at < rows.size() && compare_(rows[at].key, key) == 0;
    }
    ScratchHandle put(const Text& key, const Text& value) {
        std::size_t at = 0;
        if (find(key, at)) {
            rows[at].key = key;
            rows[at].value = value;
            return rows[at].handle;
        }
        Row row;
        row.key = key;
        row.value = value;
        row.handle = next_handle;
        rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(at), std::move(row));
        return next_handle++;
    }
    bool erase(ScratchHandle handle) {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].handle != handle) continue;
            rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
        return false;
    }
    const Row* by_handle(ScratchHandle handle) const {
        for (const Row& row : rows)
            if (row.handle == handle) return &row;
        return nullptr;
    }

    std::vector<Row> rows;
    ScratchHandle next_handle = 1;

private:
    int (*compare_)(const Text&, const Text&);
};

std::uint64_t expected_ordered_digest(const Model& model) {
    Fnv f;
    f.le64(model.rows.size());
    for (const Row& row : model.rows) {
        f.le64(row.key.size());
        f.text(row.key);
        f.le64(row.value.size());
        f.text(row.value);
    }
    return f.value;
}

std::uint64_t expected_state_digest(const Model& model, std::uint64_t order_id) {
    Fnv f;
    f.le64(order_id);
    f.le64(model.next_handle);
    f.le64(model.rows.size());
    for (const Row& row : model.rows) {
        f.le64(row.handle);
        f.le64(row.key.size());
        f.text(row.key);
        f.le64(row.value.size());
        f.text(row.value);
    }
    return f.value;
}

std::vector<Text> numeric_probes() {
    std::vector<Text> probes;
    probes.push_back(key_u64(0));
    probes.push_back(key_u64(std::numeric_limits<std::uint64_t>::max()));
    for (std::uint64_t i = 0; i < 40; ++i) probes.push_back(key_u64(mix(0xABC0000ull + i)));
    return probes;
}

// ---- backing rig: fixed memory, charged I/O, injectable faults -------------

struct Counters {
    std::uint64_t reads = 0;
    std::uint64_t writes = 0;
    std::uint64_t syncs = 0;
    static void on_charge(void* self, const ScratchIoCharge& charge) noexcept {
        auto* counters = static_cast<Counters*>(self);
        if (charge.operation == ScratchIoOperation::read_page) ++counters->reads;
        else if (charge.operation == ScratchIoOperation::write_page) ++counters->writes;
        else ++counters->syncs;
    }
    ScratchIoObserver observer() { return ScratchIoObserver{this, &Counters::on_charge}; }
};

// Wraps a fixed-memory backing and can fail one write (optionally tearing it:
// the first half of the new bytes lands, then io_error), one sync, every read,
// or every sync. It can also accept the data and full sync levels that the
// memory backing refuses, and log every write and sync attempt in order.
// Everything the store sends is charged through the observer given here.
class FaultBacking final : public ScratchBacking {
public:
    // One attempt the store made: a write of page `id`, or a sync.
    struct Event {
        char kind;
        std::uint64_t id;
    };

    FaultBacking(FixedMemoryScratchBacking& inner, ScratchIoObserver observer)
        : ScratchBacking(inner.kind(), inner.page_size(), observer), inner_(inner), merged_(inner.page_size()) {}
    std::uint64_t page_count() const noexcept override { return inner_.page_count(); }

    void arm_write(std::uint64_t nth, bool torn_write) {
        fail_write_at_ = nth;
        torn_ = torn_write;
        writes_since_arm_ = 0;
        extent_at_arm_ = inner_.page_count();
        torn_seen_ = false;
    }
    // True when the last armed write was torn onto a page that existed when it was armed.
    bool tore_existing_page() const { return torn_seen_ && torn_id_ < extent_at_arm_; }
    void disarm_write() { fail_write_at_ = 0; }
    void arm_sync(std::uint64_t nth) {
        fail_sync_at_ = nth;
        syncs_since_arm_ = 0;
    }
    void fail_reads(bool on) { fail_reads_ = on; }
    void fail_syncs(bool on) { fail_syncs_ = on; }
    void accept_durable_syncs(bool on) { durable_syncs_ = on; }
    void record_events(bool on) {
        if (on) events_.clear();
        recording_ = on;
    }
    const std::vector<Event>& events() const { return events_; }
    std::uint64_t write_faults() const { return write_faults_; }
    std::uint64_t read_faults() const { return read_faults_; }
    std::uint64_t sync_faults() const { return sync_faults_; }

protected:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer destination) const noexcept override {
        if (fail_reads_) {
            ++read_faults_;
            return ScratchStatus::io_error;
        }
        return inner_.read_page(id, destination);
    }
    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes source) noexcept override {
        if (recording_) events_.push_back(Event{'w', id});
        if (fail_write_at_ != 0) {
            ++writes_since_arm_;
            if (writes_since_arm_ == fail_write_at_) {
                fail_write_at_ = 0;
                ++write_faults_;
                if (torn_) tear(id, source);
                return ScratchStatus::io_error;
            }
        }
        return inner_.write_page(id, source);
    }
    ScratchStatus do_sync(ScratchDurability durability) noexcept override {
        if (recording_) events_.push_back(Event{'s', 0});
        if (fail_syncs_) {
            ++sync_faults_;
            return ScratchStatus::io_error;
        }
        if (fail_sync_at_ != 0 && ++syncs_since_arm_ == fail_sync_at_) {
            fail_sync_at_ = 0;
            ++sync_faults_;
            return ScratchStatus::io_error;
        }
        if (durable_syncs_ && durability != ScratchDurability::buffered) return ScratchStatus::ok;
        return inner_.sync(durability);
    }

private:
    // The first half of the new bytes lands on the old page; no allocation, so
    // a tear is also legal while the heap is closed.
    void tear(ScratchPageId id, ScratchBytes source) noexcept {
        std::fill(merged_.begin(), merged_.end(), std::byte{0});
        if (inner_.read_page(id, ScratchBuffer{merged_.data(), merged_.size()}) != ScratchStatus::ok)
            std::fill(merged_.begin(), merged_.end(), std::byte{0});
        std::memcpy(merged_.data(), source.data, merged_.size() / 2);
        (void)inner_.write_page(id, ScratchBytes{merged_.data(), merged_.size()});
        torn_id_ = id;
        torn_seen_ = true;
    }

    FixedMemoryScratchBacking& inner_;
    std::vector<std::byte> merged_;
    bool torn_ = true;
    bool torn_seen_ = false;
    bool fail_reads_ = false;
    bool fail_syncs_ = false;
    bool durable_syncs_ = false;
    bool recording_ = false;
    std::uint64_t fail_write_at_ = 0;
    std::uint64_t writes_since_arm_ = 0;
    std::uint64_t fail_sync_at_ = 0;
    std::uint64_t syncs_since_arm_ = 0;
    std::uint64_t extent_at_arm_ = 0;
    std::uint64_t torn_id_ = 0;
    std::uint64_t write_faults_ = 0;
    mutable std::uint64_t read_faults_ = 0;
    std::uint64_t sync_faults_ = 0;
    std::vector<Event> events_;
};

struct Rig {
    Rig(std::size_t page_size_arg, std::size_t pages)
        : page_size(page_size_arg), storage(page_size_arg * pages), counters(),
          memory(ScratchBuffer{storage.data(), storage.size()}, page_size_arg),
          backing(memory, counters.observer()) {}
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    std::size_t page_size;
    std::vector<std::byte> storage;
    Counters counters;
    FixedMemoryScratchBacking memory;
    FaultBacking backing;
};

ScratchStoreConfig config_for(std::size_t page_size, std::size_t page_count, std::size_t journal_slots,
                              std::size_t index_slots, const ScratchKeyOrder& order, ScratchOpenMode mode) {
    ScratchStoreConfig config;
    config.page_count = page_count;
    config.page_size = page_size;
    config.journal_slots = journal_slots;
    config.index_slots = index_slots;
    config.order = order;
    config.mode = mode;
    return config;
}

struct IoMark {
    std::uint64_t reads = 0;
    std::uint64_t writes = 0;
    std::uint64_t syncs = 0;
};

// One open store plus the I/O counters at the moment it was constructed, so
// the store's own accounting can be audited against what the backing charged.
struct Session {
    Session(Rig& rig_arg, const ScratchStoreConfig& config) : rig(rig_arg) { open(config); }
    void open(const ScratchStoreConfig& config) {
        store.reset();
        base = IoMark{rig.counters.reads, rig.counters.writes, rig.counters.syncs};
        store = std::make_unique<ScratchStore>(config, &rig.backing);
    }
    void close() { store.reset(); }
    void check_io(const char* where) const {
        require(store != nullptr, "session is open");
        const ScratchStoreStats stats = store->stats();
        const std::string w(where);
        require(stats.page_reads == rig.counters.reads - base.reads, w + ": page_reads equals the charged reads");
        require(stats.page_writes == rig.counters.writes - base.writes,
                w + ": page_writes equals the charged writes");
        require(stats.syncs == rig.counters.syncs - base.syncs, w + ": syncs equals the charged syncs");
        require(stats.backing_pages == rig.memory.page_count(), w + ": backing_pages is the backing extent");
        require(stats.backing_bytes == stats.backing_pages * rig.page_size, w + ": backing_bytes");
    }
    ScratchStore* operator->() const { return store.get(); }
    ScratchStore& operator*() const { return *store; }

    Rig& rig;
    IoMark base;
    std::unique_ptr<ScratchStore> store;
};

// ---- operations with their expected effect on the model --------------------

ScratchStatus put_text(ScratchStore& store, const Text& key, const Text& value, ScratchHandle& handle) {
    ScratchSpanSource key_source(bytes_of(key));
    ScratchSpanSource value_source(bytes_of(value));
    return store.put(key_source, value_source, handle);
}

ScratchHandle put_ok(ScratchStore& store, Model& model, const Text& key, const Text& value) {
    ScratchHandle handle = 0xDEADBEEFull;
    require_status(put_text(store, key, value, handle), ScratchStatus::ok, "put");
    require(handle == model.put(key, value), "put returns the handle the model predicts");
    return handle;
}

void erase_ok(ScratchStore& store, Model& model, ScratchHandle handle) {
    require_status(store.erase(handle), ScratchStatus::ok, "erase of a live handle");
    require(model.erase(handle), "the model held the handle");
}

void begin_ok(ScratchStore& store) { require_status(store.begin(), ScratchStatus::ok, "begin"); }
void commit_ok(ScratchStore& store) {
    require_status(store.commit(ScratchDurability::buffered), ScratchStatus::ok, "commit");
}
void rollback_ok(ScratchStore& store) { require_status(store.rollback(), ScratchStatus::ok, "rollback"); }

void build_base(ScratchStore& store, Model& model, std::size_t count, std::uint64_t seed) {
    for (std::size_t start = 0; start < count; start += 50) {
        begin_ok(store);
        const std::size_t stop = std::min<std::size_t>(start + 50, count);
        for (std::size_t i = start; i < stop; ++i) {
            const std::uint64_t k = mix(seed * 1000003ull + i);
            put_ok(store, model, key_u64(k), make_value(k, 0, static_cast<std::size_t>(mix(k) % 140)));
        }
        commit_ok(store);
    }
}

// Random edits against `pending`, which tracks every success. Returns the
// first failing status; a failed call leaves `pending` one edit behind.
ScratchStatus random_ops(ScratchStore& store, Model& pending, Rng& rng, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t pick = rng.below(100);
        if (pick < 55 || pending.rows.empty()) {
            const std::uint64_t k = rng.next();
            const Text key = key_u64(k);
            const Text value = make_value(k, 3, static_cast<std::size_t>(rng.below(260)));
            ScratchHandle handle = 0;
            const ScratchStatus status = put_text(store, key, value, handle);
            if (status != ScratchStatus::ok) return status;
            require(handle == pending.put(key, value), "random put returns the successive handle");
        } else if (pick < 75) {
            const Row row = pending.rows[static_cast<std::size_t>(rng.below(pending.rows.size()))];
            const Text value = make_value(u64_of(row.key), 5 + i, static_cast<std::size_t>(rng.below(700)));
            ScratchHandle handle = 0;
            const ScratchStatus status = put_text(store, row.key, value, handle);
            if (status != ScratchStatus::ok) return status;
            require(handle == pending.put(row.key, value) && handle == row.handle,
                    "an upsert keeps the record's handle");
        } else {
            const Row row = pending.rows[static_cast<std::size_t>(rng.below(pending.rows.size()))];
            const ScratchStatus status = store.erase(row.handle);
            if (status != ScratchStatus::ok) return status;
            require(pending.erase(row.handle), "the model held the erased handle");
        }
    }
    return ScratchStatus::ok;
}

// ---- reading the store back through the abstract interface -----------------

Text slurp(const ScratchByteSource& source, std::size_t step) {
    Text out(static_cast<std::size_t>(source.size()), '\0');
    std::size_t at = 0;
    while (at < out.size()) {
        const std::size_t n = std::min<std::size_t>(step, out.size() - at);
        require_status(source.read(at, ScratchBuffer{reinterpret_cast<std::byte*>(&out[at]), n}),
                       ScratchStatus::ok, "source read");
        at += n;
    }
    require_status(source.read(out.size(), ScratchBuffer{nullptr, 0}), ScratchStatus::ok,
                   "a zero-length read at size() is valid");
    std::byte spare{};
    require_status(source.read(out.size(), ScratchBuffer{&spare, 1}), ScratchStatus::programming_error,
                   "a read past size() is refused");
    require_status(source.read(out.size() + 1, ScratchBuffer{nullptr, 0}), ScratchStatus::programming_error,
                   "an offset past size() is refused");
    return out;
}

ScratchStatus read_text(const ScratchReadback& store, ScratchHandle handle, ScratchField field,
                        std::uint64_t offset, std::size_t length, Text& out) {
    out.assign(length, '\0');
    return store.read(handle, field, offset, ScratchBuffer{reinterpret_cast<std::byte*>(&out[0]), length});
}

struct Copy {
    ScratchHandle handle = 0;
    Text key;
    Text value;
};

struct CollectCtx {
    std::vector<Copy> rows;
    std::size_t step = 37;
    bool failed = false;
};

bool collect_rows(void* context, const ScratchRecordView& view) noexcept {
    auto* ctx = static_cast<CollectCtx*>(context);
    try {
        Copy row;
        row.handle = view.info.handle;
        row.key = slurp(view.key, ctx->step);
        row.value = slurp(view.value, ctx->step);
        if (view.info.key_bytes != row.key.size() || view.info.value_bytes != row.value.size()) ctx->failed = true;
        ctx->rows.push_back(std::move(row));
        return true;
    } catch (...) {
        ctx->failed = true;
        return false;
    }
}

std::vector<Copy> walk(const ScratchReadback& store, std::size_t page_size, std::size_t step) {
    CollectCtx ctx;
    ctx.step = step;
    std::vector<std::byte> page(page_size);
    require_status(store.for_each(ScratchBuffer{page.data(), page.size()}, &collect_rows, &ctx),
                   ScratchStatus::ok, "for_each walk");
    require(!ctx.failed, "a walked view disagreed with its own info or its source rules");
    return std::move(ctx.rows);
}

struct CountCtx {
    std::size_t calls = 0;
    std::size_t cancel_at = 0;  // 1-based; 0 never
};

bool count_visitor(void* context, const ScratchRecordView&) noexcept {
    auto* ctx = static_cast<CountCtx*>(context);
    ++ctx->calls;
    return !(ctx->cancel_at != 0 && ctx->calls == ctx->cancel_at);
}

// Chunk protocol: key chunks then value chunks, offsets contiguous from zero in
// each field, every chunk inside the caller's page, `final` exactly on the last
// chunk of a field, one empty final chunk for an empty field, nothing after a
// cancellation.
struct ChunkCtx {
    ScratchBuffer page{};
    ScratchRecordInfo expect{};
    Text key;
    Text value;
    int phase = 0;  // 0 key chunks, 1 value chunks, 2 complete
    std::uint64_t next_offset = 0;
    std::size_t calls = 0;
    std::size_t cancel_at = 0;  // 1-based; 0 never
    bool cancelled = false;
    std::string error;
};

bool chunk_collect(void* context, const ScratchChunkView& view) noexcept {
    auto* c = static_cast<ChunkCtx*>(context);
    try {
        ++c->calls;
        const auto bad = [c](const char* why) {
            if (c->error.empty()) c->error = why;
        };
        if (c->cancelled) bad("callback after cancellation");
        if (c->phase == 2) bad("callback after the final value chunk");
        if (view.info.handle != c->expect.handle || view.info.key_bytes != c->expect.key_bytes
            || view.info.value_bytes != c->expect.value_bytes)
            bad("chunk info differs from point()");
        const ScratchField want = c->phase == 0 ? ScratchField::key : ScratchField::value;
        if (view.field != want) bad("key chunks must precede value chunks");
        if (view.offset != c->next_offset) bad("offsets are not contiguous from zero");
        if (view.bytes.size > c->page.size) bad("chunk larger than the caller's page");
        if (view.bytes.size != 0
            && (view.bytes.data < c->page.data || view.bytes.data + view.bytes.size > c->page.data + c->page.size))
            bad("chunk bytes do not alias the caller's page");
        Text& sink = c->phase == 0 ? c->key : c->value;
        if (view.bytes.size != 0)
            sink.append(reinterpret_cast<const char*>(view.bytes.data), view.bytes.size);
        c->next_offset += view.bytes.size;
        const std::uint64_t total = c->phase == 0 ? c->expect.key_bytes : c->expect.value_bytes;
        if (view.final != (c->next_offset == total)) bad("final flag must mark exactly the last chunk of a field");
        if (total == 0 && !(view.offset == 0 && view.bytes.size == 0 && view.final))
            bad("an empty field is exactly one final empty chunk");
        if (total != 0 && view.bytes.size == 0) bad("an empty chunk inside a nonempty field");
        if (view.final && c->phase < 2) {
            c->phase += 1;
            c->next_offset = 0;
        }
        if (c->cancel_at != 0 && c->calls == c->cancel_at) {
            c->cancelled = true;
            return false;
        }
        return true;
    } catch (...) {
        if (c->error.empty()) c->error = "exception in chunk visitor";
        return false;
    }
}

ChunkCtx chunk_ctx_for(const ScratchReadback& store, ScratchHandle handle, ScratchBuffer page) {
    ChunkCtx ctx;
    ctx.page = page;
    require_status(store.point(handle, ctx.expect), ScratchStatus::ok, "point before visit");
    return ctx;
}

void check_visit(const ScratchReadback& store, const Row& row, std::vector<std::byte>& page) {
    ChunkCtx ctx = chunk_ctx_for(store, row.handle, ScratchBuffer{page.data(), page.size()});
    require_status(store.visit(row.handle, ctx.page, &chunk_collect, &ctx), ScratchStatus::ok, "visit");
    require(ctx.error.empty(), "visit protocol: " + ctx.error);
    require(ctx.phase == 2, "visit delivered both fields to the end");
    require(ctx.key == row.key && ctx.value == row.value, "visit chunks reassemble the record's bytes");
}

void verify_impl(const ScratchStore& store, const Model& model, std::size_t page_size,
                 const std::vector<Text>& probes, std::uint64_t order_id, bool outside_txn) {
    const std::size_t n = model.rows.size();
    require(store.record_count() == n, "record_count equals the model");

    const std::vector<Copy> rows = walk(store, page_size, 29);
    require(rows.size() == n, "for_each yields one view per live record");
    for (std::size_t i = 0; i < n; ++i) {
        require(rows[i].handle == model.rows[i].handle, "for_each handle order is the comparator order");
        require(rows[i].key == model.rows[i].key, "for_each key bytes");
        require(rows[i].value == model.rows[i].value, "for_each value bytes");
    }

    ScratchHandle at = 0xBAD;
    const ScratchStatus first_status = store.first(at);
    if (n == 0) {
        require_status(first_status, ScratchStatus::end, "first on an empty store");
        require(at == scratch_no_handle, "end writes scratch_no_handle");
    } else {
        require_status(first_status, ScratchStatus::ok, "first");
        require(at == model.rows[0].handle, "first is the smallest key");
        for (std::size_t i = 0; i + 1 < n; ++i) {
            ScratchHandle following = 0xBAD;
            require_status(store.next(model.rows[i].handle, following), ScratchStatus::ok, "next");
            require(following == model.rows[i + 1].handle, "next is the immediate ordered successor");
        }
        ScratchHandle past = 0xBAD;
        require_status(store.next(model.rows[n - 1].handle, past), ScratchStatus::end, "next past the last key");
        require(past == scratch_no_handle, "end writes scratch_no_handle after the last key");
    }

    std::vector<std::byte> page(page_size);
    const std::size_t stride = n / 48 + 1;
    for (std::size_t i = 0; i < n; i += stride) {
        const Row& row = model.rows[i];
        ScratchSpanSource probe(bytes_of(row.key));
        ScratchHandle found = 0xBAD;
        require_status(store.lower_bound(probe, found), ScratchStatus::ok, "lower_bound of a stored key");
        require(found == row.handle, "lower_bound is inclusive of equivalence");
        ScratchRecordInfo info{};
        require_status(store.point(row.handle, info), ScratchStatus::ok, "point of a live handle");
        require(info.handle == row.handle && info.key_bytes == row.key.size()
                && info.value_bytes == row.value.size(), "point reports the record's lengths");
        check_visit(store, row, page);
    }
    for (const Text& text : probes) {
        const std::size_t expected = model.lower(text);
        ScratchSpanSource probe(bytes_of(text));
        ScratchHandle found = 0xBAD;
        const ScratchStatus status = store.lower_bound(probe, found);
        if (expected == n) {
            require_status(status, ScratchStatus::end, "lower_bound past every key");
            require(found == scratch_no_handle, "end writes scratch_no_handle for a seek");
        } else {
            require_status(status, ScratchStatus::ok, "lower_bound of an absent key");
            require(found == model.rows[expected].handle, "lower_bound finds the first key not below the probe");
        }
    }

    std::uint64_t digest = 0;
    require_status(store.ordered_digest(ScratchBuffer{page.data(), page.size()}, digest), ScratchStatus::ok,
                   "ordered_digest");
    require(digest == expected_ordered_digest(model), "the ordered digest is the independent FNV fold");
    if (outside_txn) {
        std::uint64_t state = 0;
        require_status(store.state_digest(ScratchBuffer{page.data(), page.size()}, state), ScratchStatus::ok,
                       "state_digest");
        require(state == expected_state_digest(model, order_id), "the state digest is the independent FNV fold");
    }
}

void verify(const ScratchStore& store, const Model& model, std::size_t page_size,
            const std::vector<Text>& probes, std::uint64_t order_id, bool outside_txn,
            const std::string& label) {
    try {
        verify_impl(store, model, page_size, probes, order_id, outside_txn);
    } catch (const std::exception& error) {
        throw std::runtime_error(label + ": " + error.what());
    }
}

// Every mutation must be refused from inside a read or comparison callback,
// with no effect on the transaction or the data.
void refuse_mutations(ScratchStore& store, const char* where) {
    const std::string w(where);
    const bool active = store.transaction_active();
    const std::uint64_t count = store.record_count();
    const Text key = key_u64(0xFEEDFACEull);
    const Text value = "x";
    ScratchSpanSource key_source(bytes_of(key));
    ScratchSpanSource value_source(bytes_of(value));
    ScratchHandle handle = 0xBAD;
    require_status(store.begin(), ScratchStatus::programming_error, w + ": begin from a callback");
    require_status(store.put(key_source, value_source, handle), ScratchStatus::programming_error,
                   w + ": put from a callback");
    require(handle == 0xBAD, w + ": a refused put leaves its output");
    require_status(store.erase(1), ScratchStatus::programming_error, w + ": erase from a callback");
    require_status(store.commit(ScratchDurability::buffered), ScratchStatus::programming_error,
                   w + ": commit from a callback");
    require_status(store.rollback(), ScratchStatus::programming_error, w + ": rollback from a callback");
    require(store.transaction_active() == active && store.record_count() == count,
            w + ": a refused mutation changes nothing");
}

// A failed mutation leaves the transaction active and rollback-only: nothing
// but rollback is accepted, and every refusal leaves state and outputs alone.
void check_rollback_only(ScratchStore& store, std::size_t page_size) {
    require(store.transaction_active(), "a failed mutation leaves the transaction active");
    const Text key = key_u64(0xABCDEFull);
    const Text value = "late";
    ScratchSpanSource key_source(bytes_of(key));
    ScratchSpanSource value_source(bytes_of(value));
    ScratchHandle handle = 0xDEAD;
    require_status(store.put(key_source, value_source, handle), ScratchStatus::programming_error,
                   "put in a rollback-only transaction");
    require(handle == 0xDEAD, "a refused put leaves its output");
    require_status(store.erase(1), ScratchStatus::programming_error, "erase in a rollback-only transaction");
    require_status(store.commit(ScratchDurability::buffered), ScratchStatus::programming_error,
                   "commit of a rollback-only transaction");
    require_status(store.begin(), ScratchStatus::programming_error, "begin inside a rollback-only transaction");
    ScratchRecordInfo info{};
    require_status(store.point(1, info), ScratchStatus::programming_error, "point in a rollback-only transaction");
    ScratchHandle first = 0xBAD;
    require_status(store.first(first), ScratchStatus::programming_error, "first in a rollback-only transaction");
    require(first == 0xBAD, "a refused seek leaves its output");
    ScratchHandle found = 0xBAD;
    require_status(store.lower_bound(key_source, found), ScratchStatus::programming_error,
                   "lower_bound in a rollback-only transaction");
    std::vector<std::byte> page(page_size);
    std::uint64_t digest = 0x1234;
    require_status(store.ordered_digest(ScratchBuffer{page.data(), page.size()}, digest),
                   ScratchStatus::programming_error, "digest in a rollback-only transaction");
    require(digest == 0x1234, "a refused digest leaves its output");
    CountCtx count;
    require_status(store.for_each(ScratchBuffer{page.data(), page.size()}, &count_visitor, &count),
                   ScratchStatus::programming_error, "walk in a rollback-only transaction");
    require(count.calls == 0, "a refused walk delivers nothing");
    require(store.transaction_active(), "refusals do not end the transaction");
}

// ---- fixtures ---------------------------------------------------------------

void ascending_after_insert_and_erase() {
    Rig rig(512, 8192);
    OrderProbe probe;
    Session s(rig, config_for(512, 3, 1, 1, order_numeric(&probe), ScratchOpenMode::create_empty));
    require_status(s->status(), ScratchStatus::ok, "open an empty numeric store");
    Model model(cmp_numeric_text);
    const std::vector<Text> probes = numeric_probes();
    verify(*s, model, 512, probes, 1, true, "empty store");

    constexpr std::size_t kN = 1500;
    std::vector<Text> keys;
    for (std::size_t i = 0; i < kN; ++i) keys.push_back(key_u64(mix(i + 1)));
    for (std::size_t start = 0; start < kN; start += 100) {
        begin_ok(*s);
        for (std::size_t i = start; i < std::min<std::size_t>(start + 100, kN); ++i)
            put_ok(*s, model, keys[i], make_value(u64_of(keys[i]), 0, static_cast<std::size_t>(mix(i) % 90)));
        commit_ok(*s);
    }
    verify(*s, model, 512, probes, 1, true, "after shuffled inserts");

    // The caller's comparator, not byte order, decides: little-endian keys sort
    // numerically, so at least one adjacent pair must be bytewise descending.
    bool bytewise_descent = false;
    for (std::size_t i = 0; i + 1 < model.rows.size(); ++i)
        if (cmp_lex_text(model.rows[i].key, model.rows[i + 1].key) > 0) bytewise_descent = true;
    require(bytewise_descent, "the data cannot be ordered by memcmp, so the comparator is what ordered it");
    require(probe.calls >= kN - 1, "the store consulted the caller's comparator while inserting");

    // Erase every third record by handle.
    std::vector<ScratchHandle> victims;
    for (std::size_t i = 0; i < model.rows.size(); i += 3) victims.push_back(model.rows[i].handle);
    for (std::size_t start = 0; start < victims.size(); start += 100) {
        begin_ok(*s);
        for (std::size_t i = start; i < std::min<std::size_t>(start + 100, victims.size()); ++i)
            erase_ok(*s, model, victims[i]);
        commit_ok(*s);
    }
    verify(*s, model, 512, probes, 1, true, "after erasing every third record");
    for (const ScratchHandle victim : victims) {
        ScratchRecordInfo info{};
        require_status(s->point(victim, info), ScratchStatus::not_found, "an erased handle is gone");
    }
    const std::uint64_t count_before = s->record_count();
    begin_ok(*s);
    require_status(s->erase(victims[0]), ScratchStatus::not_found, "erasing an erased handle");
    require(s->record_count() == count_before, "an absent erase edits nothing");
    commit_ok(*s);

    // Upserts: same keys, new values of other sizes; handles do not move.
    Rng rng(7);
    begin_ok(*s);
    for (std::size_t i = 0; i < 200; ++i) {
        const Row row = model.rows[static_cast<std::size_t>(rng.below(model.rows.size()))];
        const std::size_t length = i % 3 == 0 ? 0 : (i % 3 == 1 ? 700 : 33);
        const ScratchHandle handle = put_ok(*s, model, row.key, make_value(u64_of(row.key), 9 + i, length));
        require(handle == row.handle, "an upsert keeps its handle");
    }
    commit_ok(*s);
    verify(*s, model, 512, probes, 1, true, "after upserts");

    // New records and erasures in one transaction.
    begin_ok(*s);
    for (std::size_t i = 0; i < 300; ++i) {
        const std::uint64_t k = mix(100000 + i);
        put_ok(*s, model, key_u64(k), make_value(k, 1, static_cast<std::size_t>(k % 50)));
    }
    for (std::size_t i = 0; i < 200; ++i) {
        const Row row = model.rows[static_cast<std::size_t>(rng.below(model.rows.size()))];
        erase_ok(*s, model, row.handle);
    }
    commit_ok(*s);
    verify(*s, model, 512, probes, 1, true, "after mixed inserts and erasures");

    // Descending comparator over the same keys: exactly the reverse order.
    Rig rig2(512, 8192);
    Session d(rig2, config_for(512, 3, 1, 1, order_numeric_desc(), ScratchOpenMode::create_empty));
    require_status(d->status(), ScratchStatus::ok, "open a descending store");
    Model descending(cmp_numeric_desc_text);
    Model ascending(cmp_numeric_text);
    begin_ok(*d);
    for (std::size_t i = 0; i < 400; ++i) {
        put_ok(*d, descending, keys[i], make_value(i, 2, i % 20));
        ascending.put(keys[i], make_value(i, 2, i % 20));
    }
    commit_ok(*d);
    verify(*d, descending, 512, probes, 5, true, "descending order");
    require(ascending.rows.size() == descending.rows.size(), "same key set");
    for (std::size_t i = 0; i < ascending.rows.size(); ++i)
        require(ascending.rows[i].key == descending.rows[ascending.rows.size() - 1 - i].key,
                "descending iteration is the exact reverse of ascending");
}

void handles_are_successive_stable_and_never_recycled() {
    Rig rig(512, 512);
    Session s(rig, config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::create_empty));
    require_status(s->status(), ScratchStatus::ok, "open");
    Model model(cmp_numeric_text);
    const std::vector<Text> probes = numeric_probes();

    begin_ok(*s);
    const std::array<std::uint64_t, 10> order{{50, 10, 90, 30, 70, 20, 100, 40, 80, 60}};
    for (std::size_t i = 0; i < order.size(); ++i) {
        ScratchHandle handle = 0;
        require_status(put_text(*s, key_u64(order[i]), "v" + std::to_string(order[i]), handle),
                       ScratchStatus::ok, "put");
        model.put(key_u64(order[i]), "v" + std::to_string(order[i]));
        require(handle == i + 1, "a fresh store hands out 1, 2, 3, ... in insertion order");
    }
    ScratchHandle again = 0;
    require_status(put_text(*s, key_u64(50), "replaced", again), ScratchStatus::ok, "upsert");
    model.put(key_u64(50), "replaced");
    require(again == 1 && s->record_count() == 10, "an upsert keeps the handle and the count");

    erase_ok(*s, model, 3);  // key 90
    erase_ok(*s, model, 7);  // key 100
    ScratchRecordInfo info{};
    require_status(s->point(3, info), ScratchStatus::not_found, "erased handle 3");
    ScratchHandle fresh = 0;
    require_status(put_text(*s, key_u64(110), "n", fresh), ScratchStatus::ok, "new record");
    model.put(key_u64(110), "n");
    require(fresh == 11, "an erased handle is never reused: the next number is 11");
    ScratchHandle readded = 0;
    require_status(put_text(*s, key_u64(90), "again", readded), ScratchStatus::ok, "re-add an erased key");
    model.put(key_u64(90), "again");
    require(readded == 12, "the re-added key gets a new handle, not its old 3");
    commit_ok(*s);

    ScratchHandle none = 0xBAD;
    require_status(s->next(3, none), ScratchStatus::not_found, "next of a dead handle");
    require(none == 0xBAD, "a refused next leaves its output");
    Text sink;
    require_status(read_text(*s, 3, ScratchField::key, 0, 0, sink), ScratchStatus::not_found, "read of a dead handle");
    require_status(s->point(0, info), ScratchStatus::not_found, "handle zero is never a record");
    require_status(s->point(1000, info), ScratchStatus::not_found, "a never-issued handle");
    require_status(s->erase(0), ScratchStatus::programming_error, "erase outside a transaction");
    begin_ok(*s);
    require_status(s->erase(0), ScratchStatus::not_found, "erase of handle zero");
    require_status(s->erase(1000), ScratchStatus::not_found, "erase of a never-issued handle");
    rollback_ok(*s);
    verify(*s, model, 512, probes, 1, true, "after handle churn");

    // A rolled-back allocation is issued again; a committed one never is.
    begin_ok(*s);
    for (std::uint64_t k = 200; k < 203; ++k) {
        ScratchHandle handle = 0;
        require_status(put_text(*s, key_u64(k), "t", handle), ScratchStatus::ok, "aborted put");
        require(handle == 13 + (k - 200), "aborted allocations continue from 13");
    }
    rollback_ok(*s);
    begin_ok(*s);
    ScratchHandle reissued = 0;
    require_status(put_text(*s, key_u64(300), "r", reissued), ScratchStatus::ok, "put after rollback");
    model.put(key_u64(300), "r");
    require(reissued == 13, "rollback restores allocation: number 13 is issued again");
    commit_ok(*s);
    verify(*s, model, 512, probes, 1, true, "after rollback reissue");
}

void forced_eviction_then_reread_by_handle() {
    Rig rig(512, 12000);
    Session s(rig, config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::create_empty));
    require_status(s->status(), ScratchStatus::ok, "open with the two-page minimum cache");
    Model model(cmp_numeric_text);
    const std::uint64_t resident_at_open = s->stats().resident_bytes;
    require(resident_at_open > 0, "the fixed workspace is accounted");

    constexpr std::size_t kN = 1500;
    std::vector<ScratchHandle> handles(kN);
    std::vector<Text> keys(kN), values(kN);
    for (std::size_t start = 0; start < kN; start += 50) {
        begin_ok(*s);
        for (std::size_t i = start; i < start + 50; ++i) {
            keys[i] = key_u64(mix(i + 77));
            const std::size_t length = i % 37 == 0 ? 1500 + static_cast<std::size_t>(mix(i) % 2500)
                                                   : static_cast<std::size_t>(mix(i) % 301);
            values[i] = make_value(i, 1, length);
            handles[i] = put_ok(*s, model, keys[i], values[i]);
        }
        commit_ok(*s);
    }
    require(rig.counters.writes >= 100, "building the set had to evict dirty pages through the backing");

    Rng rng(4242);
    std::vector<std::size_t> scramble(kN);
    for (std::size_t i = 0; i < kN; ++i) scramble[i] = i;
    for (std::size_t i = kN; i-- > 1;) std::swap(scramble[i], scramble[static_cast<std::size_t>(rng.below(i + 1))]);

    std::vector<std::byte> page(512);
    const std::uint64_t reads_before = rig.counters.reads;
    for (const std::size_t i : scramble) {
        const ScratchHandle h = handles[i];
        ScratchRecordInfo info{};
        require_status(s->point(h, info), ScratchStatus::ok, "point by handle after eviction");
        require(info.handle == h && info.key_bytes == 8 && info.value_bytes == values[i].size(),
                "point reports the stored lengths");
        Text key;
        require_status(read_text(*s, h, ScratchField::key, 0, 8, key), ScratchStatus::ok, "read key");
        require(key == keys[i], "the key survives eviction");
        Text value;
        require_status(read_text(*s, h, ScratchField::value, 0, values[i].size(), value), ScratchStatus::ok,
                       "read whole value");
        require(value == values[i], "the value survives eviction");
        const std::uint64_t length = values[i].size();
        const std::uint64_t offset = rng.below(length + 1);
        const std::size_t span = static_cast<std::size_t>(rng.below(length - offset + 1));
        Text part;
        require_status(read_text(*s, h, ScratchField::value, offset, span, part), ScratchStatus::ok,
                       "read a value range");
        require(part == values[i].substr(static_cast<std::size_t>(offset), span), "range bytes");
        if (i % 10 == 0) check_visit(*s, *model.by_handle(h), page);
    }
    require(rig.counters.reads - reads_before >= kN / 2,
            "scrambled re-reads through a two-page cache must have gone to the backing");

    const ScratchStoreStats stats = s->stats();
    require(stats.resident_bytes == resident_at_open, "the resident workspace did not grow with the data");
    require(stats.backing_pages >= 200, "the fixture holds far more pages than the cache");
    require(stats.resident_bytes < stats.backing_bytes / 2, "resident memory is small next to what it serves");
    s.check_io("after forced eviction");
    verify(*s, model, 512, numeric_probes(), 1, true, "after forced eviction");
}

struct Nested {
    ScratchStore* store = nullptr;
    const Model* model = nullptr;
    std::vector<std::byte>* chunk_page = nullptr;
    std::vector<std::byte>* walk_page = nullptr;
    std::vector<std::byte>* digest_page = nullptr;
    std::uint64_t expected_digest = 0;
    std::size_t index = 0;
    std::string error;
};

struct InnerCtx {
    std::vector<ScratchHandle> seen;
    std::size_t limit = 3;
};

bool inner_visitor(void* context, const ScratchRecordView& view) noexcept {
    auto* inner = static_cast<InnerCtx*>(context);
    try {
        inner->seen.push_back(view.info.handle);
        return inner->seen.size() < inner->limit;
    } catch (...) {
        return false;
    }
}

bool outer_visitor(void* context, const ScratchRecordView& view) noexcept {
    auto* n = static_cast<Nested*>(context);
    try {
        const std::size_t i = n->index++;
        require(i < n->model->rows.size(), "the outer walk is longer than the model");
        const Row& row = n->model->rows[i];
        require(view.info.handle == row.handle, "the outer walk keeps its order across nested calls");
        const ScratchStore& cs = *n->store;

        ScratchRecordInfo info{};
        require_status(cs.point(row.handle, info), ScratchStatus::ok, "nested point");
        require(info.key_bytes == row.key.size() && info.value_bytes == row.value.size(), "nested point lengths");
        Text via_store;
        require_status(read_text(cs, row.handle, ScratchField::key, 0, row.key.size(), via_store),
                       ScratchStatus::ok, "nested read");
        require(via_store == row.key, "nested read bytes");
        require(slurp(view.key, 5) == row.key, "the borrowed key still reads after nested calls");

        ScratchHandle found = 0xBAD;
        require_status(cs.lower_bound(view.key, found), ScratchStatus::ok,
                       "lower_bound probed with the walk's own key source");
        require(found == row.handle, "nested lower_bound finds this record");
        ScratchHandle successor = 0xBAD;
        const ScratchStatus next_status = cs.next(row.handle, successor);
        if (i + 1 < n->model->rows.size()) {
            require_status(next_status, ScratchStatus::ok, "nested next");
            require(successor == n->model->rows[i + 1].handle, "nested next is the following record");
        } else {
            require_status(next_status, ScratchStatus::end, "nested next past the last record");
            require(successor == scratch_no_handle, "nested end writes scratch_no_handle");
        }
        ScratchHandle first = 0xBAD;
        require_status(cs.first(first), ScratchStatus::ok, "nested first");
        require(first == n->model->rows[0].handle, "nested first");

        const Row& other = n->model->rows[(i * 7 + 3) % n->model->rows.size()];
        ChunkCtx chunks = chunk_ctx_for(cs, other.handle, ScratchBuffer{n->chunk_page->data(), n->chunk_page->size()});
        require_status(cs.visit(other.handle, chunks.page, &chunk_collect, &chunks), ScratchStatus::ok,
                       "nested visit of another record");
        require(chunks.error.empty(), "nested visit protocol: " + chunks.error);
        require(chunks.key == other.key && chunks.value == other.value, "nested visit bytes");

        InnerCtx inner;
        require_status(cs.for_each(ScratchBuffer{n->walk_page->data(), n->walk_page->size()}, &inner_visitor, &inner),
                       ScratchStatus::cancelled, "nested walk cancelled on its third record");
        require(inner.seen.size() == 3, "the nested walk stopped exactly at the cancellation");
        for (std::size_t k = 0; k < 3; ++k)
            require(inner.seen[k] == n->model->rows[k].handle, "the nested walk started at the first record");

        if (i % 8 == 0) {
            std::uint64_t digest = 0;
            require_status(cs.ordered_digest(ScratchBuffer{n->digest_page->data(), n->digest_page->size()}, digest),
                           ScratchStatus::ok, "nested digest");
            require(digest == n->expected_digest, "nested digest equals the model's");
        }

        refuse_mutations(*n->store, "record visitor");
        require(slurp(view.value, 11) == row.value, "the borrowed value still reads after nested calls");
        return true;
    } catch (const std::exception& error) {
        n->error = error.what();
        return false;
    } catch (...) {
        n->error = "unknown exception";
        return false;
    }
}

struct ChunkNested {
    ScratchStore* store = nullptr;
    const Model* model = nullptr;
    std::vector<std::byte>* page = nullptr;
    std::size_t calls = 0;
    std::string error;
};

bool chunk_reentrant(void* context, const ScratchChunkView& view) noexcept {
    auto* c = static_cast<ChunkNested*>(context);
    try {
        ++c->calls;
        const ScratchStore& cs = *c->store;
        ScratchRecordInfo info{};
        require_status(cs.point(view.info.handle, info), ScratchStatus::ok, "point during visit");
        require(info.handle == view.info.handle, "the same record while its chunks stream");
        const Row& other = c->model->rows[c->calls % c->model->rows.size()];
        Text sample;
        require_status(read_text(cs, other.handle, ScratchField::value, 0, std::min<std::size_t>(other.value.size(), 16),
                                 sample), ScratchStatus::ok, "read during visit");
        require(sample == other.value.substr(0, std::min<std::size_t>(other.value.size(), 16)),
                "read during visit returns the other record's bytes");
        refuse_mutations(*c->store, "chunk visitor");
        return true;
    } catch (const std::exception& error) {
        c->error = error.what();
        return false;
    } catch (...) {
        c->error = "unknown exception";
        return false;
    }
}

void reentrant_reads_during_iteration() {
    Rig rig(512, 6000);
    Session s(rig, config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::create_empty));
    require_status(s->status(), ScratchStatus::ok, "open");
    Model model(cmp_numeric_text);
    build_base(*s, model, 400, 51);
    std::vector<std::byte> p0(512), p1(512), p2(512), p3(512);

    Nested nested;
    nested.store = s.store.get();
    nested.model = &model;
    nested.chunk_page = &p1;
    nested.walk_page = &p2;
    nested.digest_page = &p3;
    nested.expected_digest = expected_ordered_digest(model);
    const std::uint64_t reads_before = rig.counters.reads;
    require_status(s->for_each(ScratchBuffer{p0.data(), p0.size()}, &outer_visitor, &nested), ScratchStatus::ok,
                   "outer walk with nested reads");
    require(nested.error.empty(), "nested reads: " + nested.error);
    require(nested.index == model.rows.size(), "the outer walk visited every record exactly once");
    require(rig.counters.reads - reads_before > model.rows.size(), "nested reads really went to the backing");
    verify(*s, model, 512, numeric_probes(), 1, true, "after the reentrant walk");

    // The same inside an open transaction: pending edits are visible, refused
    // mutations do not poison it, and it still commits.
    begin_ok(*s);
    Model pending = model;
    for (std::uint64_t k = 0; k < 30; ++k) put_ok(*s, pending, key_u64(mix(9000 + k)), make_value(k, 4, 20 + k));
    Nested inside;
    inside.store = s.store.get();
    inside.model = &pending;
    inside.chunk_page = &p1;
    inside.walk_page = &p2;
    inside.digest_page = &p3;
    inside.expected_digest = expected_ordered_digest(pending);
    require_status(s->for_each(ScratchBuffer{p0.data(), p0.size()}, &outer_visitor, &inside), ScratchStatus::ok,
                   "outer walk inside a transaction");
    require(inside.error.empty(), "nested reads in a transaction: " + inside.error);
    require(inside.index == pending.rows.size(), "the transactional walk saw the pending edits");
    require(s->transaction_active(), "refused callbacks did not end the transaction");

    // Chunk visitors may also read and are refused the same mutations.
    ChunkNested chunk;
    chunk.store = s.store.get();
    chunk.model = &pending;
    chunk.page = &p1;
    const Row& target = pending.rows[pending.rows.size() / 2];
    require_status(s->visit(target.handle, ScratchBuffer{p0.data(), p0.size()}, &chunk_reentrant, &chunk),
                   ScratchStatus::ok, "visit with reentrant reads");
    require(chunk.error.empty(), "reentrant chunk visitor: " + chunk.error);
    require(chunk.calls >= 2, "the record streamed key and value chunks");
    commit_ok(*s);
    verify(*s, pending, 512, numeric_probes(), 1, true, "committed after refused callbacks");
}

void comparator_cannot_mutate_the_store() {
    Rig rig(512, 512);
    OrderProbe probe;
    Session s(rig, config_for(512, 2, 1, 1, order_numeric(&probe), ScratchOpenMode::create_empty));
    require_status(s->status(), ScratchStatus::ok, "open");
    Model model(cmp_numeric_text);
    begin_ok(*s);
    put_ok(*s, model, key_u64(5), "five");
    probe.victim = s.store.get();
    put_ok(*s, model, key_u64(7), "seven");
    require(probe.attempted, "the second put had to compare against the first record");
    probe.victim = nullptr;
    for (const ScratchStatus status : probe.attempts)
        require_status(status, ScratchStatus::programming_error, "mutation from inside a comparison");
    put_ok(*s, model, key_u64(6), "six");
    commit_ok(*s);
    verify(*s, model, 512, numeric_probes(), 1, true, "after a comparator tried to mutate");
}

void rollback_after_partial_writes() {
    Rig rig(512, 12000);
    const std::vector<Text> probes = numeric_probes();
    Session s(rig, config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::create_empty));
    require_status(s->status(), ScratchStatus::ok, "open");
    Model model(cmp_numeric_text);
    build_base(*s, model, 300, 41);
    const Model before = model;
    verify(*s, before, 512, probes, 1, true, "committed base");

    Model pending = model;
    const std::uint64_t writes_before = rig.counters.writes;
    begin_ok(*s);
    Rng rng(88);
    require_status(random_ops(*s, pending, rng, 500), ScratchStatus::ok, "edits inside the transaction");
    require(rig.counters.writes - writes_before >= 40,
            "the partial edits spilled through the two-page cache to the backing before the rollback");
    verify(*s, pending, 512, probes, 1, false, "pending edits are visible inside the transaction");
    rollback_ok(*s);
    require(!s->transaction_active(), "rollback ends the transaction");
    verify(*s, before, 512, probes, 1, true, "after rollback");
    ScratchRecordInfo info{};
    require_status(s->point(before.next_handle, info), ScratchStatus::not_found,
                   "a handle issued only by the aborted transaction is gone");

    begin_ok(*s);
    ScratchHandle reissued = 0;
    require_status(put_text(*s, key_u64(mix(7777)), "after", reissued), ScratchStatus::ok, "put after rollback");
    require(reissued == before.next_handle, "allocation restarts exactly at the pre-begin value");
    rollback_ok(*s);

    // Erase everything, observe the empty store inside, roll back.
    begin_ok(*s);
    for (const Row& row : before.rows) require_status(s->erase(row.handle), ScratchStatus::ok, "mass erase");
    Model empty(cmp_numeric_text);
    empty.next_handle = before.next_handle;
    verify(*s, empty, 512, probes, 1, false, "everything erased inside the transaction");
    rollback_ok(*s);
    verify(*s, before, 512, probes, 1, true, "after rolling back a mass erase");
    s.check_io("after rollbacks");
}

// The nth backing write of a long transaction is refused, clean or torn. The
// commit phase has its own sweep (failed_commit_publishes_nothing). Returns
// whether a torn write landed on a page that existed before the transaction.
bool torn_write_during_transaction(std::uint64_t nth, bool torn, bool explicit_rollback) {
    Rig rig(512, 6000);
    const std::vector<Text> probes = numeric_probes();
    Session s(rig, config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::create_empty));
    require_status(s->status(), ScratchStatus::ok, "open");
    Model model(cmp_numeric_text);
    build_base(*s, model, 200, 11);
    const Model before = model;
    verify(*s, before, 512, probes, 1, true, "committed base");

    Model pending = model;
    Rng rng(1000 + nth);
    begin_ok(*s);
    rig.backing.arm_write(nth, torn);
    const ScratchStatus failure = random_ops(*s, pending, rng, 600);
    require(rig.backing.write_faults() == 1, "the injected write fault was reached inside the edits");
    require_status(failure, ScratchStatus::io_error, "a mutation preserves the backing's io_error");
    const bool damaged = torn && rig.backing.tore_existing_page();
    check_rollback_only(*s, 512);
    if (explicit_rollback) {
        rollback_ok(*s);
        verify(*s, before, 512, probes, 1, true, "after rolling back a torn write");
        begin_ok(*s);
        Model next = before;
        put_ok(*s, next, key_u64(mix(31337)), "still usable");
        commit_ok(*s);
        verify(*s, next, 512, probes, 1, true, "usable after a torn write");
        return damaged;
    }
    s.close();
    s.open(config_for(512, 4, 2, 1, order_numeric(), ScratchOpenMode::reopen));
    require_status(s->status(), ScratchStatus::ok, "reopen after an abandoned rollback-only transaction");
    verify(*s, before, 512, probes, 1, true, "reopen sees the last commit, not the torn transaction");
    return damaged;
}

void rollback_survives_torn_writes_at_every_phase() {
    const std::array<std::uint64_t, 12> positions{{1, 2, 3, 4, 6, 9, 14, 21, 34, 55, 89, 144}};
    std::size_t damaged = 0;
    for (const std::uint64_t nth : positions) {
        if (torn_write_during_transaction(nth, true, true)) ++damaged;
        torn_write_during_transaction(nth, false, true);
        if (torn_write_during_transaction(nth, true, false)) ++damaged;
    }
    require(damaged >= 6, "torn writes really landed on pages that existed before the transaction");
}

struct CommitRun {
    bool failed = false;
    std::uint64_t writes = 0;
};

// The same committed base and the same edits every time, so that the fault
// positions are the commit's own write numbers. nth == 0 injects nothing. The
// last write of a commit is its superblock, which a torn write lands whole.
CommitRun commit_fault_run(std::uint64_t nth, bool torn, bool explicit_rollback) {
    CommitRun run;
    const std::vector<Text> probes = numeric_probes();
    Rig rig(512, 6000);
    Session s(rig, config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::create_empty));
    require_status(s->status(), ScratchStatus::ok, "open");
    Model model(cmp_numeric_text);
    build_base(*s, model, 150, 21);
    const Model before = model;
    begin_ok(*s);
    Model pending = model;
    Rng rng(500);
    require_status(random_ops(*s, pending, rng, 25), ScratchStatus::ok, "edits before the commit");
    if (nth != 0) rig.backing.arm_write(nth, torn);
    const std::uint64_t writes_before = rig.counters.writes;
    const ScratchStatus status = s->commit(ScratchDurability::buffered);
    run.writes = rig.counters.writes - writes_before;
    if (status == ScratchStatus::ok) {
        require(rig.backing.write_faults() == 0, "a commit that succeeded did not meet a fault");
        verify(*s, pending, 512, probes, 1, true, "after the commit");
        return run;
    }
    run.failed = true;
    require(rig.backing.write_faults() == 1, "only the injected fault can fail this commit");
    require_status(status, ScratchStatus::io_error, "commit reports the failing backing call");
    check_rollback_only(*s, 512);
    if (explicit_rollback) {
        rollback_ok(*s);
        verify(*s, before, 512, probes, 1, true, "rolled back after a failed commit");
    }
    s.close();
    s.open(config_for(512, 5, 2, 2, order_numeric(), ScratchOpenMode::reopen));
    require_status(s->status(), ScratchStatus::ok, "reopen after a failed commit");
    verify(*s, before, 512, probes, 1, true, "a failed commit published nothing");
    return run;
}

void failed_commit_publishes_nothing() {
    const CommitRun good = commit_fault_run(0, false, true);
    require(!good.failed && good.writes >= 2, "an unfaulted commit succeeds and writes at least a journal link and a superblock");
    for (const bool explicit_rollback : {true, false}) {
        for (const bool torn : {false, true}) {
            for (std::uint64_t nth = 1; nth <= good.writes; ++nth) {
                const CommitRun run = commit_fault_run(nth, torn, explicit_rollback);
                require(run.failed, "a refused write inside the commit fails it");
            }
            const CommitRun beyond = commit_fault_run(good.writes + 1, torn, explicit_rollback);
            require(!beyond.failed, "a fault past the commit's last write is never reached");
        }
    }
}

void durability_failure_and_transaction_state_machine() {
    Rig rig(512, 1024);
    Session s(rig, config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::create_empty));
    require_status(s->status(), ScratchStatus::ok, "open");
    Model model(cmp_numeric_text);
    const std::vector<Text> probes = numeric_probes();

    ScratchHandle handle = 0xDEAD;
    require_status(put_text(*s, key_u64(1), "x", handle), ScratchStatus::programming_error, "put outside a transaction");
    require(handle == 0xDEAD, "a refused put leaves its output");
    require_status(s->erase(1), ScratchStatus::programming_error, "erase outside a transaction");
    require_status(s->commit(ScratchDurability::buffered), ScratchStatus::programming_error, "commit without begin");
    require_status(s->rollback(), ScratchStatus::programming_error, "rollback without begin");
    require(!s->transaction_active(), "nothing is active yet");
    verify(*s, model, 512, probes, 1, true, "reads work outside a transaction");

    begin_ok(*s);
    require(s->transaction_active(), "begin activates");
    require_status(s->begin(), ScratchStatus::programming_error, "nested begin");
    require(s->transaction_active(), "a refused nested begin does not disturb the transaction");
    require_status(s->commit(static_cast<ScratchDurability>(9)), ScratchStatus::programming_error,
                   "an unknown durability level");
    require(s->transaction_active(), "a refused durability level does not poison the transaction");
    put_ok(*s, model, key_u64(1), "one");
    commit_ok(*s);
    require(!s->transaction_active(), "commit ends the transaction");

    // Memory is not durable: asking for data durability fails, publishes
    // nothing, and leaves a rollback-only transaction.
    begin_ok(*s);
    Model pending = model;
    put_ok(*s, pending, key_u64(2), "two");
    require_status(s->commit(ScratchDurability::data), ScratchStatus::programming_error,
                   "data durability on a memory backing reports the backing's refusal");
    check_rollback_only(*s, 512);
    rollback_ok(*s);
    verify(*s, model, 512, probes, 1, true, "after a refused durable commit");
    s.close();
    s.open(config_for(512, 3, 1, 1, order_numeric(), ScratchOpenMode::reopen));
    require_status(s->status(), ScratchStatus::ok, "reopen");
    verify(*s, model, 512, probes, 1, true, "the refused durable commit published nothing");

    // An empty transaction commits, and an absent erase does not poison.
    begin_ok(*s);
    commit_ok(*s);
    begin_ok(*s);
    require_status(s->erase(999), ScratchStatus::not_found, "absent erase");
    put_ok(*s, model, key_u64(3), "three");
    commit_ok(*s);
    verify(*s, model, 512, probes, 1, true, "after an empty commit and an absent erase");
}

void expect_unusable(ScratchStore& store, ScratchStatus open_status, const char* why) {
    const std::string w(why);
    require_status(store.status(), open_status, w + ": open status");
    const ScratchStoreStats before = store.stats();
    require(!store.transaction_active() && store.record_count() == 0, w + ": an unusable store holds nothing");
    require_status(store.begin(), ScratchStatus::programming_error, w + ": begin");
    ScratchHandle first = 0xBAD;
    require_status(store.first(first), ScratchStatus::programming_error, w + ": first");
    require(first == 0xBAD, w + ": output untouched");
    std::vector<std::byte> page(512);
    CountCtx count;
    require_status(store.for_each(ScratchBuffer{page.data(), page.size()}, &count_visitor, &count),
                   ScratchStatus::programming_error, w + ": for_each");
    require(count.calls == 0, w + ": no callback");
    std::uint64_t digest = 0x77;
    require_status(store.ordered_digest(ScratchBuffer{page.data(), page.size()}, digest),
                   ScratchStatus::programming_error, w + ": digest");
    require(digest == 0x77, w + ": digest output untouched");
    std::uint64_t state = 0x78;
    require_status(store.state_digest(ScratchBuffer{page.data(), page.size()}, state),
                   ScratchStatus::programming_error, w + ": state digest");
    require(state == 0x78, w + ": state digest output untouched");

    const Text key = key_u64(1);
    ScratchSpanSource key_source(bytes_of(key));
    ScratchHandle handle = 0xBAD;
    require_status(store.put(key_source, key_source, handle), ScratchStatus::programming_error, w + ": put");
    require(handle == 0xBAD, w + ": put output untouched");
    require_status(store.erase(1), ScratchStatus::programming_error, w + ": erase");
    require_status(store.commit(ScratchDurability::buffered), ScratchStatus::programming_error, w + ": commit");
    require_status(store.rollback(), ScratchStatus::programming_error, w + ": rollback");
    ScratchRecordInfo info{};
    require_status(store.point(1, info), ScratchStatus::programming_error, w + ": point");
    std::array<std::byte, 4> part{};
    require_status(store.read(1, ScratchField::key, 0, ScratchBuffer{part.data(), part.size()}),
                   ScratchStatus::programming_error, w + ": read");
    ScratchHandle next = 0xBAD;
    require_status(store.next(1, next), ScratchStatus::programming_error, w + ": next");
    require(next == 0xBAD, w + ": next output untouched");
    ScratchHandle found = 0xBAD;
    require_status(store.lower_bound(key_source, found), ScratchStatus::programming_error, w + ": lower_bound");
    require(found == 0xBAD, w + ": lower_bound output untouched");
    const auto no_chunks = [](void*, const ScratchChunkView&) noexcept -> bool { return true; };
    require_status(store.visit(1, ScratchBuffer{page.data(), page.size()}, no_chunks, nullptr),
                   ScratchStatus::programming_error, w + ": visit");
    const ScratchStoreStats after = store.stats();
    require(after.page_reads == before.page_reads && after.page_writes == before.page_writes
                && after.syncs == before.syncs,
            w + ": calls on an unusable store issued no backing call");
}

void commit_then_reopen_and_abandon() {
    Rig rig(512, 12000);
    const std::vector<Text> probes = numeric_probes();
    Model model(cmp_numeric_text);
    std::uint64_t ordered_before = 0, state_before = 0;
    std::vector<std::byte> page(512);
    std::vector<ScratchHandle> erased;
    {
        Session a(rig, config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::create_empty));
        require_status(a->status(), ScratchStatus::ok, "create");
        build_base(*a, model, 600, 31);
        begin_ok(*a);
        for (std::size_t i = 0; i < model.rows.size(); i += 4) erased.push_back(model.rows[i].handle);
        for (const ScratchHandle h : erased) erase_ok(*a, model, h);
        commit_ok(*a);
        begin_ok(*a);
        Rng rng(5);
        for (std::size_t i = 0; i < 100; ++i) {
            const Row row = model.rows[static_cast<std::size_t>(rng.below(model.rows.size()))];
            put_ok(*a, model, row.key, make_value(u64_of(row.key), 70 + i, 300 + i));
        }
        commit_ok(*a);
        verify(*a, model, 512, probes, 1, true, "before close");
        require_status(a->ordered_digest(ScratchBuffer{page.data(), page.size()}, ordered_before), ScratchStatus::ok,
                       "digest before close");
        require_status(a->state_digest(ScratchBuffer{page.data(), page.size()}, state_before), ScratchStatus::ok,
                       "state digest before close");
        a.check_io("create session");
    }
    {
        Session b(rig, config_for(512, 9, 3, 2, order_numeric(), ScratchOpenMode::reopen));
        require_status(b->status(), ScratchStatus::ok, "reopen with a different cache geometry");
        verify(*b, model, 512, probes, 1, true, "after reopen");
        std::uint64_t ordered_after = 0, state_after = 0;
        require_status(b->ordered_digest(ScratchBuffer{page.data(), page.size()}, ordered_after), ScratchStatus::ok,
                       "digest after reopen");
        require_status(b->state_digest(ScratchBuffer{page.data(), page.size()}, state_after), ScratchStatus::ok,
                       "state digest after reopen");
        require(ordered_after == ordered_before && state_after == state_before,
                "digests are identical across close and reopen");
        for (const ScratchHandle h : erased) {
            ScratchRecordInfo info{};
            require_status(b->point(h, info), ScratchStatus::not_found, "an erased handle stays erased across reopen");
        }
        begin_ok(*b);
        const ScratchHandle expected_next = model.next_handle;
        ScratchHandle fresh = 0;
        require_status(put_text(*b, key_u64(mix(999999)), "fresh", fresh), ScratchStatus::ok, "put after reopen");
        model.put(key_u64(mix(999999)), "fresh");
        require(fresh == expected_next, "handle allocation continues after reopen and never recycles");
        commit_ok(*b);
        verify(*b, model, 512, probes, 1, true, "after a post-reopen commit");
        b.check_io("reopen session");
    }
    const Model committed = model;
    {
        Session c(rig, config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::reopen));
        require_status(c->status(), ScratchStatus::ok, "reopen to abandon a transaction");
        Model pending = model;
        Rng rng(77);
        const std::uint64_t writes_before = rig.counters.writes;
        begin_ok(*c);
        require_status(random_ops(*c, pending, rng, 400), ScratchStatus::ok, "edits in the abandoned transaction");
        require(rig.counters.writes - writes_before >= 20, "the abandoned edits reached the backing");
    }
    {
        Session d(rig, config_for(512, 4, 1, 3, order_numeric(), ScratchOpenMode::reopen));
        require_status(d->status(), ScratchStatus::ok, "reopen after an abandoned transaction");
        verify(*d, committed, 512, probes, 1, true, "an abandoned transaction yields the last commit");
    }

    // Open failures: every one is a status, none touches state, none is usable.
    {
        Rig empty(512, 8);
        ScratchStore store(config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::reopen), &empty.backing);
        expect_unusable(store, ScratchStatus::not_found, "reopen of an empty backing");
        require(empty.counters.writes == 0, "a refused open writes nothing");
    }
    {
        ScratchStore store(config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::create_empty), &rig.backing);
        expect_unusable(store, ScratchStatus::programming_error, "create_empty on a used backing");
    }
    {
        ScratchKeyOrder wrong = order_numeric();
        wrong.id = 77;
        ScratchStore store(config_for(512, 2, 1, 1, wrong, ScratchOpenMode::reopen), &rig.backing);
        expect_unusable(store, ScratchStatus::programming_error, "reopen under another ordering id");
    }
    {
        ScratchStore store(config_for(1024, 2, 1, 1, order_numeric(), ScratchOpenMode::reopen), &rig.backing);
        expect_unusable(store, ScratchStatus::programming_error, "page size differs from the backing's");
    }
    {
        Rig garbage(512, 16);
        std::vector<std::byte> junk(512, std::byte{0xA5});
        for (ScratchPageId id = 0; id < 6; ++id)
            require_status(garbage.backing.write_page(id, ScratchBytes{junk.data(), junk.size()}), ScratchStatus::ok,
                           "fill garbage");
        ScratchStore store(config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::reopen), &garbage.backing);
        expect_unusable(store, ScratchStatus::corrupt, "reopen of garbage");
    }
    {
        Rig never(512, 4000);
        {
            Session u(never, config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::create_empty));
            require_status(u->status(), ScratchStatus::ok, "create without ever committing");
            Model scratch(cmp_numeric_text);
            Rng rng(3);
            begin_ok(*u);
            require_status(random_ops(*u, scratch, rng, 300), ScratchStatus::ok, "uncommitted edits");
        }
        require(never.memory.page_count() > 0, "the uncommitted edits reached the backing");
        ScratchStore store(config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::reopen), &never.backing);
        expect_unusable(store, ScratchStatus::corrupt, "reopen of a store that never committed");
    }
}

struct Step {
    bool erase = false;
    Text key;
    Text value;
    ScratchHandle handle = 0;
};

std::vector<Step> make_steps(std::size_t count, Model& model) {
    Rng rng(2024);
    std::vector<Step> steps;
    for (std::size_t i = 0; i < count; ++i) {
        Step step;
        if (!model.rows.empty() && rng.below(100) < 30) {
            const Row row = model.rows[static_cast<std::size_t>(rng.below(model.rows.size()))];
            step.erase = true;
            step.key = row.key;
            step.handle = row.handle;
            model.erase(row.handle);
        } else {
            const std::uint64_t k = rng.below(400);
            step.key = key_u64(k * 2654435761ull);
            step.value = make_value(k, i, static_cast<std::size_t>(rng.below(200)));
            step.handle = model.put(step.key, step.value);
        }
        steps.push_back(std::move(step));
    }
    return steps;
}

struct Shape {
    std::size_t page_size;
    std::size_t page_count;
    std::size_t journal_slots;
    std::size_t index_slots;
    std::size_t batch;
    std::size_t pages;
};

void run_steps(const Shape& shape, const std::vector<Step>& steps, const Model& final_model,
               std::uint64_t& ordered_out, std::uint64_t& state_out, const std::string& label) {
    Rig rig(shape.page_size, shape.pages);
    Session s(rig, config_for(shape.page_size, shape.page_count, shape.journal_slots, shape.index_slots,
                              order_numeric(), ScratchOpenMode::create_empty));
    require_status(s->status(), ScratchStatus::ok, label + ": open");
    std::size_t in_txn = 0;
    for (const Step& step : steps) {
        if (in_txn == 0) begin_ok(*s);
        if (step.erase) {
            require_status(s->erase(step.handle), ScratchStatus::ok, label + ": erase step");
        } else {
            ScratchHandle handle = 0;
            require_status(put_text(*s, step.key, step.value, handle), ScratchStatus::ok, label + ": put step");
            require(handle == step.handle, label + ": handle allocation is independent of geometry and batching");
        }
        if (++in_txn == shape.batch) {
            commit_ok(*s);
            in_txn = 0;
        }
    }
    if (in_txn != 0) commit_ok(*s);
    verify(*s, final_model, shape.page_size, numeric_probes(), 1, true, label);
    std::vector<std::byte> page(shape.page_size);
    require_status(s->ordered_digest(ScratchBuffer{page.data(), page.size()}, ordered_out), ScratchStatus::ok,
                   label + ": ordered digest");
    require_status(s->state_digest(ScratchBuffer{page.data(), page.size()}, state_out), ScratchStatus::ok,
                   label + ": state digest");
}

void digest_is_throughput_neutral() {
    Model model(cmp_numeric_text);
    const std::vector<Step> steps = make_steps(1200, model);
    const std::uint64_t expected_ordered = expected_ordered_digest(model);
    const std::uint64_t expected_state = expected_state_digest(model, 1);
    const std::array<Shape, 6> shapes{{
        {512, 2, 1, 1, 1, 12000},
        {512, 5, 2, 2, 7, 12000},
        {1024, 3, 1, 1, 100, 6000},
        {4096, 64, 4, 4, 1200, 1800},
        {4096, 2, 1, 1, 33, 1800},
        {512, 64, 1, 1, 1200, 12000},
    }};
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        const std::string label = "shape " + std::to_string(i);
        std::uint64_t ordered = 0, state = 0;
        run_steps(shapes[i], steps, model, ordered, state, label);
        require(ordered == expected_ordered, label + ": ordered digest independent of page size, cache, journal and batching");
        require(state == expected_state, label + ": state digest independent of the same knobs");
    }
    std::uint64_t ordered_again = 0, state_again = 0;
    run_steps(shapes[0], steps, model, ordered_again, state_again, "replay");
    require(ordered_again == expected_ordered && state_again == expected_state,
            "an identical operation sequence yields identical digests");
}

void digest_covers_every_stored_byte() {
    Rig rig(512, 1024);
    Session s(rig, config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::create_empty));
    require_status(s->status(), ScratchStatus::ok, "open");
    Model model(cmp_numeric_text);
    begin_ok(*s);
    for (std::uint64_t k = 1; k <= 50; ++k) put_ok(*s, model, key_u64(k), make_value(k, 0, 20 + k));
    commit_ok(*s);
    std::vector<std::byte> page(512);
    std::uint64_t base = 0, base_state = 0;
    require_status(s->ordered_digest(ScratchBuffer{page.data(), page.size()}, base), ScratchStatus::ok, "digest");
    require_status(s->state_digest(ScratchBuffer{page.data(), page.size()}, base_state), ScratchStatus::ok, "state");
    require(base == expected_ordered_digest(model), "baseline equals the oracle");

    Row victim = model.rows[17];
    Text flipped = victim.value;
    flipped.back() = static_cast<char>(flipped.back() ^ 0x01);
    begin_ok(*s);
    put_ok(*s, model, victim.key, flipped);
    commit_ok(*s);
    std::uint64_t changed = 0;
    require_status(s->ordered_digest(ScratchBuffer{page.data(), page.size()}, changed), ScratchStatus::ok, "digest");
    require(changed != base && changed == expected_ordered_digest(model), "one flipped value bit changes the digest");
    begin_ok(*s);
    put_ok(*s, model, victim.key, victim.value);
    commit_ok(*s);
    std::uint64_t restored = 0;
    require_status(s->ordered_digest(ScratchBuffer{page.data(), page.size()}, restored), ScratchStatus::ok, "digest");
    require(restored == base, "restoring the bytes restores the digest");

    begin_ok(*s);
    put_ok(*s, model, victim.key, victim.value + "!");
    commit_ok(*s);
    std::uint64_t longer = 0;
    require_status(s->ordered_digest(ScratchBuffer{page.data(), page.size()}, longer), ScratchStatus::ok, "digest");
    require(longer != base, "a longer value changes the digest");
    begin_ok(*s);
    put_ok(*s, model, victim.key, victim.value);
    commit_ok(*s);

    // Content digest ignores handles; the state digest does not.
    begin_ok(*s);
    erase_ok(*s, model, victim.handle);
    put_ok(*s, model, victim.key, victim.value);
    commit_ok(*s);
    std::uint64_t reissued = 0, reissued_state = 0;
    require_status(s->ordered_digest(ScratchBuffer{page.data(), page.size()}, reissued), ScratchStatus::ok, "digest");
    require_status(s->state_digest(ScratchBuffer{page.data(), page.size()}, reissued_state), ScratchStatus::ok,
                   "state");
    require(reissued == base, "the ordered digest does not depend on handles");
    require(reissued_state != base_state && reissued_state == expected_state_digest(model, 1),
            "the state digest covers handles and the allocation counter");
    std::uint64_t in_txn = 0x4242;
    begin_ok(*s);
    require_status(s->state_digest(ScratchBuffer{page.data(), page.size()}, in_txn), ScratchStatus::programming_error,
                   "state_digest is only legal outside a transaction");
    require(in_txn == 0x4242, "a refused digest leaves its output");
    rollback_ok(*s);
}

constexpr std::size_t kSlots = 600;
// Above a few hundred bytes a 512-byte page store keeps a value as a chain of
// whole pages, so the closed-heap workload reaches both kinds of storage.
constexpr std::size_t kMaxValue = 1300;

struct HashCtx {
    Fnv fnv;
    bool failed = false;
};

bool fold_source(Fnv& fnv, const ScratchByteSource& source) noexcept {
    std::array<std::byte, 48> chunk;
    for (std::uint64_t at = 0; at < source.size();) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), source.size() - at));
        if (source.read(at, ScratchBuffer{chunk.data(), n}) != ScratchStatus::ok) return false;
        fnv.raw(chunk.data(), n);
        at += n;
    }
    return true;
}

bool hash_visitor(void* context, const ScratchRecordView& view) noexcept {
    auto* ctx = static_cast<HashCtx*>(context);
    ctx->fnv.le64(view.info.key_bytes);
    if (!fold_source(ctx->fnv, view.key)) {
        ctx->failed = true;
        return false;
    }
    ctx->fnv.le64(view.info.value_bytes);
    if (!fold_source(ctx->fnv, view.value)) {
        ctx->failed = true;
        return false;
    }
    return true;
}

struct VisitCtx {
    Fnv fnv;
};

bool visit_hash_chunk(void* context, const ScratchChunkView& view) noexcept {
    static_cast<VisitCtx*>(context)->fnv.raw(view.bytes.data, view.bytes.size);
    return true;
}

// Runs against the real store with no heap available: nothing here allocates.
struct StoreTarget {
    ScratchStore& store;
    ScratchBuffer page;
    std::uint64_t failures = 0;

    void begin() {
        if (store.begin() != ScratchStatus::ok) ++failures;
    }
    void commit() {
        if (store.commit(ScratchDurability::buffered) != ScratchStatus::ok) ++failures;
    }
    void rollback() {
        if (store.rollback() != ScratchStatus::ok) ++failures;
    }
    ScratchHandle put(std::uint64_t key, const std::byte* value, std::size_t length) {
        std::array<std::byte, 8> raw{};
        store_le64(raw.data(), key);
        ScratchSpanSource key_source(ScratchBytes{raw.data(), raw.size()});
        ScratchSpanSource value_source(ScratchBytes{value, length});
        ScratchHandle handle = 0;
        if (store.put(key_source, value_source, handle) != ScratchStatus::ok) {
            ++failures;
            return 0;
        }
        return handle;
    }
    void erase(ScratchHandle handle) {
        if (store.erase(handle) != ScratchStatus::ok) ++failures;
    }
    std::uint64_t value_length(ScratchHandle handle) {
        ScratchRecordInfo info{};
        if (store.point(handle, info) != ScratchStatus::ok) {
            ++failures;
            return 0;
        }
        return info.value_bytes;
    }
    ScratchHandle lower_bound_handle(std::uint64_t key) {
        std::array<std::byte, 8> raw{};
        store_le64(raw.data(), key);
        ScratchSpanSource probe(ScratchBytes{raw.data(), raw.size()});
        ScratchHandle handle = 0;
        const ScratchStatus status = store.lower_bound(probe, handle);
        if (status != ScratchStatus::ok && status != ScratchStatus::end) ++failures;
        return handle;
    }
    std::uint64_t walk_hash() {
        HashCtx ctx;
        if (store.for_each(page, &hash_visitor, &ctx) != ScratchStatus::ok || ctx.failed) ++failures;
        return ctx.fnv.value;
    }
    std::uint64_t digest() {
        std::uint64_t value = 0;
        if (store.ordered_digest(page, value) != ScratchStatus::ok) ++failures;
        return value;
    }
    std::uint64_t state_digest() {
        std::uint64_t value = 0;
        if (store.state_digest(page, value) != ScratchStatus::ok) ++failures;
        return value;
    }
    // Up to 40 value bytes at an offset inside the value, folded with the offset.
    std::uint64_t read_hash(ScratchHandle handle, std::uint64_t offset) {
        ScratchRecordInfo info{};
        if (store.point(handle, info) != ScratchStatus::ok) {
            ++failures;
            return 0;
        }
        const std::uint64_t at = info.value_bytes == 0 ? 0 : offset % info.value_bytes;
        std::array<std::byte, 40> part{};
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(part.size(), info.value_bytes - at));
        if (store.read(handle, ScratchField::value, at, ScratchBuffer{part.data(), n}) != ScratchStatus::ok) {
            ++failures;
            return 0;
        }
        Fnv f;
        f.le64(at);
        f.raw(part.data(), n);
        return f.value;
    }
    // The key bytes and then the value bytes, whatever the chunking.
    std::uint64_t visit_hash(ScratchHandle handle) {
        VisitCtx ctx;
        if (store.visit(handle, page, &visit_hash_chunk, &ctx) != ScratchStatus::ok) ++failures;
        return ctx.fnv.value;
    }
    ScratchHandle first_handle() {
        ScratchHandle handle = 0;
        const ScratchStatus status = store.first(handle);
        if (status != ScratchStatus::ok && status != ScratchStatus::end) ++failures;
        return handle;
    }
    ScratchHandle next_handle(ScratchHandle current) {
        ScratchHandle handle = 0;
        const ScratchStatus status = store.next(current, handle);
        if (status != ScratchStatus::ok && status != ScratchStatus::end) ++failures;
        return handle;
    }
};

struct ModelTarget {
    Model model{cmp_numeric_text};
    std::vector<Row> snapshot_rows;
    ScratchHandle snapshot_next = 1;

    void begin() {
        snapshot_rows = model.rows;
        snapshot_next = model.next_handle;
    }
    void commit() {}
    void rollback() {
        model.rows = snapshot_rows;
        model.next_handle = snapshot_next;
    }
    ScratchHandle put(std::uint64_t key, const std::byte* value, std::size_t length) {
        return model.put(key_u64(key), Text(reinterpret_cast<const char*>(value), length));
    }
    void erase(ScratchHandle handle) { model.erase(handle); }
    std::uint64_t value_length(ScratchHandle handle) {
        const Row* row = model.by_handle(handle);
        return row != nullptr ? row->value.size() : 0;
    }
    ScratchHandle lower_bound_handle(std::uint64_t key) {
        const std::size_t at = model.lower(key_u64(key));
        return at < model.rows.size() ? model.rows[at].handle : 0;
    }
    std::uint64_t walk_hash() {
        Fnv f;
        for (const Row& row : model.rows) {
            f.le64(row.key.size());
            f.text(row.key);
            f.le64(row.value.size());
            f.text(row.value);
        }
        return f.value;
    }
    std::uint64_t digest() { return expected_ordered_digest(model); }
    std::uint64_t state_digest() { return expected_state_digest(model, 1); }
    std::uint64_t read_hash(ScratchHandle handle, std::uint64_t offset) {
        const Row* row = model.by_handle(handle);
        if (row == nullptr) return 0;
        const std::uint64_t size = row->value.size();
        const std::uint64_t at = size == 0 ? 0 : offset % size;
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(40, size - at));
        Fnv f;
        f.le64(at);
        f.text(row->value.substr(static_cast<std::size_t>(at), n));
        return f.value;
    }
    std::uint64_t visit_hash(ScratchHandle handle) {
        const Row* row = model.by_handle(handle);
        Fnv f;
        if (row != nullptr) {
            f.text(row->key);
            f.text(row->value);
        }
        return f.value;
    }
    ScratchHandle first_handle() { return model.rows.empty() ? 0 : model.rows.front().handle; }
    ScratchHandle next_handle(ScratchHandle current) {
        for (std::size_t i = 0; i + 1 < model.rows.size(); ++i)
            if (model.rows[i].handle == current) return model.rows[i + 1].handle;
        return 0;
    }
};

// One generator for both targets, so a divergence is a store defect and never
// a difference between two copies of the workload.
template <class Target>
std::uint64_t run_workload(Target& t, std::uint64_t seed, std::size_t ops) {
    Rng rng(seed);
    std::array<ScratchHandle, kSlots> slot{};
    std::array<ScratchHandle, kSlots> saved{};
    std::array<std::byte, kMaxValue> value{};
    Fnv trace;
    bool open = false;
    for (std::size_t op = 0; op < ops; ++op) {
        if (!open) {
            t.begin();
            saved = slot;
            open = true;
        }
        const std::uint64_t pick = rng.below(100);
        const std::size_t k = static_cast<std::size_t>(rng.below(kSlots));
        const std::uint64_t key = k * 7919 + 13;
        if (pick < 50) {
            const std::size_t length = static_cast<std::size_t>(rng.below(kMaxValue + 1));
            fill_bytes(value.data(), length, rng.next());
            slot[k] = t.put(key, value.data(), length);
            trace.le64(slot[k]);
        } else if (pick < 70) {
            if (slot[k] != 0) {
                t.erase(slot[k]);
                slot[k] = 0;
                trace.le64(1);
            } else {
                trace.le64(0);
            }
        } else if (pick < 78) {
            trace.le64(slot[k] != 0 ? t.value_length(slot[k]) : 99);
        } else if (pick < 84) {
            trace.le64(t.lower_bound_handle(key));
        } else if (pick < 86) {
            trace.le64(t.walk_hash());
        } else if (pick < 88) {
            trace.le64(t.digest());
        } else if (pick < 92) {
            const std::uint64_t offset = rng.next();
            trace.le64(slot[k] != 0 ? t.read_hash(slot[k], offset) : 98);
        } else if (pick < 96) {
            trace.le64(slot[k] != 0 ? t.visit_hash(slot[k]) : 97);
        } else {
            ScratchHandle at = t.first_handle();
            for (int step = 0; step < 3 && at != 0; ++step) {
                trace.le64(at);
                at = t.next_handle(at);
            }
            trace.le64(at);
        }
        if (op % 40 == 39) {
            if (rng.below(4) == 0) {
                t.rollback();
                slot = saved;
                trace.le64(2);
            } else {
                t.commit();
                trace.le64(3);
            }
            open = false;
            trace.le64(t.state_digest());
        }
    }
    if (open) {
        t.commit();
        trace.le64(t.state_digest());
    }
    trace.le64(t.digest());
    return trace.value;
}

void no_heap_after_construction() {
    Rig rig(512, 8192);
    Session s(rig, config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::create_empty));
    require_status(s->status(), ScratchStatus::ok, "open");
    std::vector<std::byte> page(512);
    StoreTarget target{*s, ScratchBuffer{page.data(), page.size()}};
    const std::uint64_t resident_before = s->stats().resident_bytes;
    const std::size_t allocations_before = global_allocation::allocations;
    const std::size_t refused_before = global_allocation::refused;

    global_allocation::refusing = true;
    const std::uint64_t store_trace = run_workload(target, 20260101ull, 6000);
    global_allocation::refusing = false;

    require(global_allocation::refused == refused_before,
            "no operation attempted to allocate: there is no growth and no bad_alloc path");
    require(global_allocation::allocations == allocations_before, "the whole workload made zero heap allocations");
    require(target.failures == 0, "every operation succeeded with the heap closed");
    require(s->stats().resident_bytes == resident_before, "the resident workspace is constant");
    ModelTarget oracle;
    const std::uint64_t model_trace = run_workload(oracle, 20260101ull, 6000);
    require(store_trace == model_trace, "the store's handles, reads and digests match the oracle step for step");
    verify(*s, oracle.model, 512, numeric_probes(), 1, true, "after the heap-closed workload");
    require(rig.counters.writes >= 200, "the heap-closed workload really spilled through a two-page cache");
}

// "Bounded" can only be tested as a plateau on a finite run: the extent may keep
// a fixed allowance for rare larger transactions after the measurement point,
// but it must not grow in proportion to the work done. The allowances below are
// this fixture's measure of that, not a limit the contract states.
void space_is_reused_under_churn() {
    Rig rig(512, 1024);
    Session s(rig, config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::create_empty));
    require_status(s->status(), ScratchStatus::ok, "open");
    Model model(cmp_numeric_text);
    Rng rng(99);
    begin_ok(*s);
    for (std::uint64_t k = 0; k < 64; ++k) put_ok(*s, model, key_u64(k), make_value(k, 0, 40 + k % 60));
    commit_ok(*s);
    std::uint64_t next_key = 1000;
    std::uint64_t quarter_extent = 0;
    std::uint64_t late_extent = 0;
    std::uint64_t churned_bytes = 0;
    constexpr std::size_t kTxns = 3750;
    for (std::size_t t = 0; t < kTxns; ++t) {
        begin_ok(*s);
        for (int op = 0; op < 8; ++op) {
            const Row row = model.rows[static_cast<std::size_t>(rng.below(model.rows.size()))];
            const std::size_t length = 30 + static_cast<std::size_t>(rng.below(120));
            if (rng.below(100) < 50) {
                put_ok(*s, model, row.key, make_value(u64_of(row.key), t, length));
            } else {
                erase_ok(*s, model, row.handle);
                put_ok(*s, model, key_u64(next_key), make_value(next_key, t, length));
                ++next_key;
            }
            churned_bytes += 8 + length;
        }
        commit_ok(*s);
        if (t % 400 == 399) {
            begin_ok(*s);
            for (std::uint64_t i = 0; i < 8; ++i) {
                ScratchHandle handle = 0;
                require_status(put_text(*s, key_u64(mix(t * 10 + i)), make_value(t, i, 90), handle), ScratchStatus::ok,
                               "put in a transaction that will roll back");
            }
            rollback_ok(*s);
        }
        if (t == kTxns / 4) quarter_extent = s->stats().backing_pages;
        if (t == 3 * kTxns / 4) late_extent = s->stats().backing_pages;
    }
    const std::uint64_t end_extent = s->stats().backing_pages;
    require(rig.counters.writes > kTxns, "every one of the commits wrote to the backing");
    require(churned_bytes > 3 * rig.memory.capacity_pages() * rig.page_size,
            "the logical bytes written exceed the whole fixed backing several times over, so only reuse survives");
    require(end_extent <= quarter_extent + quarter_extent / 4 + 16,
            "under steady churn of a bounded live set the backing extent plateaus");
    require(end_extent <= late_extent + 12, "the last quarter of the churn added next to nothing");
    require(model.rows.size() == 64 && s->record_count() == 64, "the live set stayed at 64 records");
    verify(*s, model, 512, numeric_probes(), 1, true, "after 30000 churn operations");
}

void rollback_does_not_leak_space() {
    Rig rig(512, 1024);
    Session s(rig, config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::create_empty));
    require_status(s->status(), ScratchStatus::ok, "open");
    Model model(cmp_numeric_text);
    build_base(*s, model, 64, 5);
    std::uint64_t early = 0;
    for (std::uint64_t cycle = 0; cycle < 3000; ++cycle) {
        begin_ok(*s);
        for (std::uint64_t i = 0; i < 12; ++i) {
            ScratchHandle handle = 0;
            require_status(put_text(*s, key_u64(mix(cycle * 100 + i + 7)), make_value(cycle, i, 100), handle),
                           ScratchStatus::ok, "put in a transaction that will roll back");
        }
        rollback_ok(*s);
        if (cycle == 99) early = s->stats().backing_pages;
    }
    const std::uint64_t late = s->stats().backing_pages;
    require(late <= early + early / 4 + 16, "repeated begin/put/rollback does not grow the backing without bound");
    verify(*s, model, 512, numeric_probes(), 1, true, "after 3000 rolled-back transactions");
}

// Handle numbers are never reissued, so a live set that keeps replacing its
// records walks through handle numbers without end. Whatever maps those numbers
// to records must be given back behind the live range, or the backing grows
// with the handles ever issued although the live set does not. A 512-byte page
// maps a few dozen handles, so twenty thousand of them are hundreds of pages.
void handle_table_space_follows_the_live_range() {
    Rig rig(512, 1024);
    Session s(rig, config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::create_empty));
    require_status(s->status(), ScratchStatus::ok, "open");
    Model model(cmp_numeric_text);
    begin_ok(*s);
    for (std::uint64_t k = 0; k < 8; ++k) put_ok(*s, model, key_u64(k), make_value(k, 0, 40));
    commit_ok(*s);
    constexpr std::size_t kRounds = 20000;
    std::uint64_t next_key = 8;
    std::uint64_t quarter_extent = 0;
    for (std::size_t round = 0; round < kRounds; ++round) {
        begin_ok(*s);
        erase_ok(*s, model, model.rows.front().handle);
        put_ok(*s, model, key_u64(next_key), make_value(next_key, round, 40));
        ++next_key;
        commit_ok(*s);
        if (round == kRounds / 4) quarter_extent = s->stats().backing_pages;
    }
    const std::uint64_t end_extent = s->stats().backing_pages;
    require(model.next_handle > kRounds, "more handle numbers were issued than the whole backing has pages for");
    require(rig.counters.writes > kRounds, "every replacement reached the backing");
    require(quarter_extent != 0, "the extent was sampled a quarter of the way through");
    require(end_extent <= quarter_extent + 4, "the backing extent follows the live range, not the handles ever issued");
    require(model.rows.size() == 8 && s->record_count() == 8, "the live set stayed at eight records");
    verify(*s, model, 512, numeric_probes(), 1, true, "after twenty thousand replacements");
}

struct RetryRun {
    std::uint64_t commit_writes = 0;
    std::uint64_t rollback_writes = 0;
    std::uint64_t faults = 0;
};

// A store whose journal pool is already populated, then a transaction that
// spills through a two-page cache. commit_fault_at == 0 only counts the writes
// of a good commit. Otherwise the commit's own write number commit_fault_at (its
// superblock) is refused, which leaves a published-looking superblock behind,
// and rollback_fault_at refuses that write number of the rollback, torn or
// clean. A rollback that fails must be retryable, and after the retry the
// store must hold exactly its last committed state, also across a reopen.
RetryRun rollback_retry_run(std::uint64_t commit_fault_at, std::uint64_t rollback_fault_at, bool torn) {
    RetryRun run;
    Rig rig(512, 1024);
    Session s(rig, config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::create_empty));
    require_status(s->status(), ScratchStatus::ok, "open");
    Model model(cmp_numeric_text);
    const std::vector<Text> probes = numeric_probes();
    build_base(*s, model, 40, 11);
    Rng rng(5);
    begin_ok(*s);
    require_status(random_ops(*s, model, rng, 60), ScratchStatus::ok, "edits that fill the journal pool");
    commit_ok(*s);

    Model pending = model;
    begin_ok(*s);
    require_status(random_ops(*s, pending, rng, 60), ScratchStatus::ok, "edits that will be undone");
    if (commit_fault_at == 0) {
        const std::uint64_t before = rig.counters.writes;
        commit_ok(*s);
        run.commit_writes = rig.counters.writes - before;
        verify(*s, pending, 512, probes, 1, true, "after the counted commit");
        return run;
    }
    rig.backing.arm_write(commit_fault_at, true);
    require_status(s->commit(ScratchDurability::buffered), ScratchStatus::io_error, "the superblock write is refused");
    require(rig.backing.write_faults() == 1, "the commit's fault fired");
    check_rollback_only(*s, 512);
    if (rollback_fault_at != 0) rig.backing.arm_write(rollback_fault_at, torn);
    const std::uint64_t before = rig.counters.writes;
    ScratchStatus status = s->rollback();
    if (status != ScratchStatus::ok) {
        require_status(status, ScratchStatus::io_error, "a refused write is reported as the backing gave it");
        run.faults = rig.backing.write_faults() - 1;
        require(s->transaction_active(), "a rollback that failed leaves the transaction active");
        check_rollback_only(*s, 512);
        status = s->rollback();
    }
    require_status(status, ScratchStatus::ok, "the retried rollback completes");
    run.rollback_writes = rig.counters.writes - before;
    require(!s->transaction_active(), "the retried rollback ends the transaction");
    verify(*s, model, 512, probes, 1, true, "after the retried rollback");
    s.close();
    s.open(config_for(512, 3, 1, 1, order_numeric(), ScratchOpenMode::reopen));
    require_status(s->status(), ScratchStatus::ok, "reopen after the retried rollback");
    verify(*s, model, 512, probes, 1, true, "a reopen sees only the last successful commit");
    return run;
}

void rollback_can_be_retried_after_a_refused_write() {
    const RetryRun counted = rollback_retry_run(0, 0, false);
    require(counted.commit_writes >= 2, "a commit writes at least the journal link and the superblock");
    const RetryRun clean = rollback_retry_run(counted.commit_writes, 0, false);
    require(clean.faults == 0 && clean.rollback_writes >= 6,
            "an unfaulted rollback restores several pages and blanks the superblock it found");
    std::uint64_t fired = 0;
    for (const bool torn : {false, true}) {
        for (std::uint64_t nth = 1; nth <= clean.rollback_writes; ++nth) {
            const RetryRun run = rollback_retry_run(counted.commit_writes, nth, torn);
            require(run.faults == 1, "a refused write fired at every position of the rollback");
            fired += run.faults;
        }
    }
    require(fired == 2 * clean.rollback_writes, "every position of the rollback was refused once, torn and clean");
}

// data and full ask the medium for durability after every journal and data
// write and before the superblock that publishes, and once more after it. The
// memory backing refuses those levels, so this rig accepts them and logs every
// write and sync the commit issues.
void durable_commit_publishes_between_two_syncs() {
    for (const ScratchDurability level : {ScratchDurability::data, ScratchDurability::full}) {
        Rig rig(512, 512);
        rig.backing.accept_durable_syncs(true);
        Session s(rig, config_for(512, 3, 1, 1, order_numeric(), ScratchOpenMode::create_empty));
        require_status(s->status(), ScratchStatus::ok, "open");
        Model model(cmp_numeric_text);
        for (std::uint64_t round = 0; round < 4; ++round) {
            begin_ok(*s);
            for (std::uint64_t k = 0; k < 25; ++k)
                put_ok(*s, model, key_u64(round * 1000 + k), make_value(k, round, 70 + k));
            rig.backing.record_events(true);
            require_status(s->commit(level), ScratchStatus::ok, "a durable commit on a backing that accepts the level");
            rig.backing.record_events(false);
            const std::vector<FaultBacking::Event>& events = rig.backing.events();
            std::size_t syncs = 0;
            std::size_t first_sync = events.size();
            std::size_t last_sync = 0;
            std::size_t superblock_writes = 0;
            std::size_t superblock_at = 0;
            for (std::size_t i = 0; i < events.size(); ++i) {
                if (events[i].kind == 's') {
                    if (syncs++ == 0) first_sync = i;
                    last_sync = i;
                } else if (events[i].id < 2) {
                    ++superblock_writes;
                    superblock_at = i;
                }
            }
            require(syncs == 2, "a durable commit asks the medium to sync twice");
            require(superblock_writes == 1, "exactly one superblock write publishes the commit");
            require(first_sync > 0, "journal and data pages are written before the first sync");
            for (std::size_t i = 0; i < first_sync; ++i)
                require(events[i].kind == 'w' && events[i].id >= 2, "only journal and data pages precede the first sync");
            require(first_sync < superblock_at && superblock_at < last_sync, "the superblock is written between the syncs");
            require(last_sync + 1 == events.size(), "nothing follows the final sync");
        }
        verify(*s, model, 512, numeric_probes(), 1, true, "after four durable commits");
        s.close();
        s.open(config_for(512, 4, 1, 1, order_numeric(), ScratchOpenMode::reopen));
        require_status(s->status(), ScratchStatus::ok, "reopen after durable commits");
        verify(*s, model, 512, numeric_probes(), 1, true, "a reopen sees the durable commits");
    }

    // A refusal of the final sync leaves the new superblock on the medium, so
    // the commit must report it and rollback must take the publication back.
    Rig rig(512, 512);
    rig.backing.accept_durable_syncs(true);
    Session s(rig, config_for(512, 3, 1, 1, order_numeric(), ScratchOpenMode::create_empty));
    require_status(s->status(), ScratchStatus::ok, "open");
    Model model(cmp_numeric_text);
    const std::vector<Text> probes = numeric_probes();
    begin_ok(*s);
    for (std::uint64_t k = 0; k < 20; ++k) put_ok(*s, model, key_u64(k), make_value(k, 0, 60));
    commit_ok(*s);
    begin_ok(*s);
    Model pending = model;
    for (std::uint64_t k = 100; k < 120; ++k) put_ok(*s, pending, key_u64(k), make_value(k, 1, 60));
    rig.backing.arm_sync(2);
    require_status(s->commit(ScratchDurability::data), ScratchStatus::io_error,
                   "a refused final sync is reported as the backing gave it");
    require(rig.backing.sync_faults() == 1, "the final sync was the call that failed");
    check_rollback_only(*s, 512);
    rollback_ok(*s);
    verify(*s, model, 512, probes, 1, true, "after the refused final sync is rolled back");
    s.close();
    s.open(config_for(512, 4, 1, 1, order_numeric(), ScratchOpenMode::reopen));
    require_status(s->status(), ScratchStatus::ok, "reopen after the refused final sync");
    verify(*s, model, 512, probes, 1, true, "the refused publication was taken back");

    // A refusal of the first sync comes before anything is published.
    begin_ok(*s);
    pending = model;
    for (std::uint64_t k = 200; k < 215; ++k) put_ok(*s, pending, key_u64(k), make_value(k, 2, 60));
    rig.backing.fail_syncs(true);
    require_status(s->commit(ScratchDurability::full), ScratchStatus::io_error,
                   "a refused first sync is reported as the backing gave it");
    rig.backing.fail_syncs(false);
    require(rig.backing.sync_faults() == 2, "the refused first sync was the call that failed");
    check_rollback_only(*s, 512);
    rollback_ok(*s);
    verify(*s, model, 512, probes, 1, true, "after the refused first sync is rolled back");
    s.close();
    s.open(config_for(512, 3, 1, 1, order_numeric(), ScratchOpenMode::reopen));
    require_status(s->status(), ScratchStatus::ok, "reopen after the refused first sync");
    verify(*s, model, 512, probes, 1, true, "the refused first sync published nothing");
}

void keys_and_values_larger_than_the_cache() {
    Rig rig(512, 12000);
    OrderProbe probe;
    Session s(rig, config_for(512, 2, 1, 1, order_lex(&probe), ScratchOpenMode::create_empty));
    require_status(s->status(), ScratchStatus::ok, "open a lexicographic store");
    Model model(cmp_lex_text);

    std::vector<Text> keys;
    for (const std::size_t len : {0u, 1u, 63u, 64u, 65u, 511u, 512u, 513u, 1025u})
        keys.push_back(make_value(len + 1, 9, len));
    const Text base = pattern_text(3000, 55);
    for (std::size_t j = 0; j < 24; ++j) {
        Text key = base;
        key[2999] = static_cast<char>(j);
        key[2998] = static_cast<char>(j / 8);
        keys.push_back(key);
    }
    keys.push_back(base.substr(0, 2000));
    keys.push_back(base.substr(0, 2000) + "tail");

    const std::array<std::size_t, 7> sizes{{0, 1, 511, 512, 513, 5000, 40000}};
    begin_ok(*s);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const std::size_t size = sizes[i % sizes.size()];
        if (i % 5 == 4) {
            // A value that is only a formula: the store must pull it in pieces.
            ScratchSpanSource key_source(bytes_of(keys[i]));
            PatternSource value_source(size, 1000 + i);
            ScratchHandle handle = 0;
            require_status(s->put(key_source, value_source, handle), ScratchStatus::ok, "put from a formula source");
            require(handle == model.put(keys[i], pattern_text(size, 1000 + i)), "formula-sourced handle");
            const std::uint64_t resident = s->stats().resident_bytes;
            require(value_source.max_request <= resident, "no request was larger than the store's whole workspace");
            if (size > resident)
                require(value_source.reads >= 2, "a value larger than the whole workspace was pulled in pieces");
        } else {
            put_ok(*s, model, keys[i], pattern_text(size, 1000 + i));
        }
    }
    commit_ok(*s);
    require(probe.max_end > 512, "comparisons had to read keys beyond one page");
    const std::vector<Text> probes{Text(), Text(1, '\xff'), base.substr(0, 1000), base, base.substr(0, 2000) + "u"};
    verify(*s, model, 512, probes, 2, true, "keys and values larger than the cache");

    // Boundary-straddling ranges against the oracle, and refused ranges.
    for (const Row& row : model.rows) {
        if (row.value.size() < 513) continue;
        const std::uint64_t size = row.value.size();
        const std::array<std::uint64_t, 10> offsets{{0, 1, 510, 511, 512, 513, 1023, 1024, size - 1, size}};
        for (const std::uint64_t offset : offsets) {
            for (const std::size_t length : {0u, 1u, 2u, 511u, 512u, 513u, 1025u}) {
                if (offset + length > size) continue;
                Text part;
                require_status(read_text(*s, row.handle, ScratchField::value, offset, length, part), ScratchStatus::ok,
                               "boundary read");
                require(part == row.value.substr(static_cast<std::size_t>(offset), length), "boundary bytes");
            }
        }
        Text sink;
        require_status(read_text(*s, row.handle, ScratchField::value, size + 1, 0, sink),
                       ScratchStatus::programming_error, "offset past the value");
        require_status(read_text(*s, row.handle, ScratchField::value, size, 1, sink),
                       ScratchStatus::programming_error, "range past the value");
        require_status(s->read(row.handle, ScratchField::value, 0, ScratchBuffer{nullptr, 1}),
                       ScratchStatus::programming_error, "null destination");
    }
}

void equal_keys_upsert_and_key_bytes() {
    Rig rig(512, 512);
    Session s(rig, config_for(512, 2, 1, 1, order_fold(), ScratchOpenMode::create_empty));
    require_status(s->status(), ScratchStatus::ok, "open a case-folding store");
    Model model(cmp_fold_text);
    const std::vector<Text> probes{Text(), "a", "alpha", "Alpha", "beta", "zzz"};
    begin_ok(*s);
    const ScratchHandle a = put_ok(*s, model, "Alpha", "v1");
    const ScratchHandle b = put_ok(*s, model, "beta", "bb");
    const ScratchHandle a2 = put_ok(*s, model, "ALPHA", "v2");
    require(a == 1 && b == 2 && a2 == a, "comparator-equal keys are one record with one handle");
    require(s->record_count() == 2, "equal keys do not add records");
    commit_ok(*s);
    verify(*s, model, 512, probes, 4, true, "case-folded records");
    require(model.rows[0].key == "ALPHA" && model.rows[0].value == "v2",
            "the record takes the latest put's key bytes and value");

    const Text lower = "alpha";
    ScratchSpanSource probe(bytes_of(lower));
    ScratchHandle found = 0;
    require_status(s->lower_bound(probe, found), ScratchStatus::ok, "seek with another case");
    require(found == a, "a differently cased probe finds the equal record");

    std::vector<std::byte> page(512);
    std::uint64_t d1 = 0, d2 = 0, d3 = 0;
    require_status(s->ordered_digest(ScratchBuffer{page.data(), page.size()}, d1), ScratchStatus::ok, "digest");
    begin_ok(*s);
    put_ok(*s, model, "alpha", "v2");
    commit_ok(*s);
    require_status(s->ordered_digest(ScratchBuffer{page.data(), page.size()}, d2), ScratchStatus::ok, "digest");
    require(d2 != d1 && d2 == expected_ordered_digest(model), "the digest covers key bytes, not just order");
    verify(*s, model, 512, probes, 4, true, "after re-casing a key");
    begin_ok(*s);
    put_ok(*s, model, "ALPHA", "v2");
    commit_ok(*s);
    require_status(s->ordered_digest(ScratchBuffer{page.data(), page.size()}, d3), ScratchStatus::ok, "digest");
    require(d3 == d1, "restoring the key bytes restores the digest");

    // The empty key and the empty value are ordinary records.
    begin_ok(*s);
    const ScratchHandle empty = put_ok(*s, model, "", "");
    commit_ok(*s);
    require(empty == 3 && model.rows[0].key.empty(), "the empty key sorts first and has a handle");
    begin_ok(*s);
    erase_ok(*s, model, empty);
    const ScratchHandle again = put_ok(*s, model, "", "");
    commit_ok(*s);
    require(again == 4, "re-adding the empty key issues a new handle");
    verify(*s, model, 512, probes, 4, true, "with the empty key");
}

void misuse_and_error_surface() {
    Rig rig(512, 4096);
    OrderProbe probe;
    Session s(rig, config_for(512, 2, 1, 1, order_numeric(&probe), ScratchOpenMode::create_empty));
    require_status(s->status(), ScratchStatus::ok, "open");
    Model model(cmp_numeric_text);
    std::vector<std::byte> page(512), short_page(511), long_page(513);

    // The empty store: end, no calls, an ok digest.
    {
        ScratchHandle h = 0xBAD;
        require_status(s->first(h), ScratchStatus::end, "first on empty");
        require(h == scratch_no_handle, "end writes scratch_no_handle");
        const Text k = key_u64(5);
        ScratchSpanSource ks(bytes_of(k));
        h = 0xBAD;
        require_status(s->lower_bound(ks, h), ScratchStatus::end, "seek on empty");
        require(h == scratch_no_handle, "end writes scratch_no_handle for a seek");
        CountCtx count;
        require_status(s->for_each(ScratchBuffer{page.data(), page.size()}, &count_visitor, &count), ScratchStatus::ok,
                       "empty walk");
        require(count.calls == 0, "an empty stream makes no callback");
    }

    begin_ok(*s);
    for (std::uint64_t k = 0; k < 100; ++k) put_ok(*s, model, key_u64(k * 3), make_value(k, 0, 64));
    const Text big = make_value(1, 1, 2000);
    const ScratchHandle big_handle = put_ok(*s, model, key_u64(1000), big);
    commit_ok(*s);

    // Buffer misuse: no callback, outputs untouched.
    CountCtx none;
    for (const ScratchBuffer bad : {ScratchBuffer{short_page.data(), short_page.size()},
                                    ScratchBuffer{long_page.data(), long_page.size()}, ScratchBuffer{nullptr, 512}}) {
        require_status(s->for_each(bad, &count_visitor, &none), ScratchStatus::programming_error, "for_each page");
        require(none.calls == 0, "a refused walk makes no callback");
        ChunkCtx chunks;
        chunks.page = bad;
        require_status(s->visit(model.rows[0].handle, bad, &chunk_collect, &chunks), ScratchStatus::programming_error,
                       "visit page");
        require(chunks.calls == 0, "a refused visit makes no callback");
        std::uint64_t digest = 0x99;
        require_status(s->ordered_digest(bad, digest), ScratchStatus::programming_error, "digest page");
        require_status(s->state_digest(bad, digest), ScratchStatus::programming_error, "state digest page");
        require(digest == 0x99, "a refused digest leaves its output");
    }

    // Absent handles.
    {
        ChunkCtx chunks;
        chunks.page = ScratchBuffer{page.data(), page.size()};
        require_status(s->visit(123456, chunks.page, &chunk_collect, &chunks), ScratchStatus::not_found, "visit absent");
        require(chunks.calls == 0, "no callback for an absent handle");
        ScratchRecordInfo info;
        info.handle = 0x7777;
        info.key_bytes = 0x7777;
        info.value_bytes = 0x7777;
        require_status(s->point(123456, info), ScratchStatus::not_found, "point absent");
        require(info.handle == 0x7777 && info.key_bytes == 0x7777 && info.value_bytes == 0x7777,
                "a refused point leaves info unchanged");
    }

    // Cancellation at every position, including the last record and chunk.
    for (std::size_t at = 1; at <= model.rows.size(); ++at) {
        CountCtx count;
        count.cancel_at = at;
        require_status(s->for_each(ScratchBuffer{page.data(), page.size()}, &count_visitor, &count),
                       ScratchStatus::cancelled, "walk cancelled");
        require(count.calls == at, "no callback after a cancellation, even on the last record");
    }
    {
        ChunkCtx probe_chunks = chunk_ctx_for(*s, big_handle, ScratchBuffer{page.data(), page.size()});
        require_status(s->visit(big_handle, probe_chunks.page, &chunk_collect, &probe_chunks), ScratchStatus::ok,
                       "count the chunks");
        const std::size_t total = probe_chunks.calls;
        require(total >= 5 && probe_chunks.error.empty(), "a 2000-byte value streams in several chunks");
        for (std::size_t at = 1; at <= total; ++at) {
            ChunkCtx chunks = chunk_ctx_for(*s, big_handle, ScratchBuffer{page.data(), page.size()});
            chunks.cancel_at = at;
            require_status(s->visit(big_handle, chunks.page, &chunk_collect, &chunks), ScratchStatus::cancelled,
                           "visit cancelled");
            require(chunks.calls == at && chunks.error.empty(), "visit stops at the cancelling chunk");
        }
    }

    // A failing medium is io_error, never an empty success or a cancellation.
    {
        // A reopened store has an empty cache, so the walk must go to the medium.
        s.close();
        s.open(config_for(512, 2, 1, 1, order_numeric(&probe), ScratchOpenMode::reopen));
        require_status(s->status(), ScratchStatus::ok, "reopen before the failing medium");
        const std::uint64_t faults_before = rig.backing.read_faults();
        rig.backing.fail_reads(true);
        CountCtx count;
        const ScratchStatus walk_status = s->for_each(ScratchBuffer{page.data(), page.size()}, &count_visitor, &count);
        std::uint64_t digest = 0x55;
        const ScratchStatus digest_status = s->ordered_digest(ScratchBuffer{page.data(), page.size()}, digest);
        rig.backing.fail_reads(false);
        require_status(walk_status, ScratchStatus::io_error, "a failing medium fails the walk");
        require(count.calls < model.rows.size(), "the failed walk did not deliver a full stream");
        require_status(digest_status, ScratchStatus::io_error, "a failing medium fails the digest");
        require(digest == 0x55, "a failed digest yields no sentinel value");
        require(rig.backing.read_faults() > faults_before, "the read fault was reached");
        verify(*s, model, 512, numeric_probes(), 1, true, "the store recovers once the medium does");
    }

    // A failing comparator: seeks return its status, puts fail as programming errors.
    {
        const Text k = key_u64(150);
        ScratchSpanSource ks(bytes_of(k));
        ScratchHandle h = 0xBAD;
        probe.fail_from_call = probe.calls + 1;
        require_status(s->lower_bound(ks, h), ScratchStatus::io_error, "comparator failure surfaces in a seek");
        require(h == 0xBAD, "a failed seek leaves its output");
        probe.fail_from_call = 0;
        begin_ok(*s);
        probe.fail_from_call = probe.calls + 1;
        ScratchHandle put_result = 0xDEAD;
        require_status(put_text(*s, key_u64(151), "x", put_result), ScratchStatus::programming_error,
                       "comparator failure fails the put");
        require(put_result == 0xDEAD, "a failed put leaves its output");
        probe.fail_from_call = 0;
        check_rollback_only(*s, 512);
        rollback_ok(*s);
        verify(*s, model, 512, numeric_probes(), 1, true, "after a comparator failure");
    }
}

// A source that is asked for its bytes and cannot give them.
class FailingSource final : public ScratchByteSource {
public:
    explicit FailingSource(std::uint64_t size) : size_(size) {}
    std::uint64_t size() const noexcept override { return size_; }
    ScratchStatus read(std::uint64_t, ScratchBuffer) const noexcept override {
        ++reads;
        return ScratchStatus::io_error;
    }
    mutable std::uint64_t reads = 0;

private:
    std::uint64_t size_;
};

// There is no capacity status. These three causes remain programming_error:
// an exhausted backing, a source that fails to deliver its bytes, and no
// memory for the workspace. A backing io_error is preserved instead; either
// failed mutation leaves its transaction rollback-only.
void named_failure_causes_are_programming_errors() {
    const std::vector<Text> probes = numeric_probes();
    {
        Rig rig(512, 6);
        Session s(rig, config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::create_empty));
        require_status(s->status(), ScratchStatus::ok, "open over a six-page backing");
        begin_ok(*s);
        ScratchStatus failure = ScratchStatus::ok;
        std::uint64_t served = 0;
        for (std::uint64_t k = 0; k < 400 && failure == ScratchStatus::ok; ++k) {
            ScratchHandle handle = 0;
            failure = put_text(*s, key_u64(k), make_value(k, 0, 90), handle);
            if (failure == ScratchStatus::ok) ++served;
        }
        require(served >= 1, "the first put fits in six pages");
        require_status(failure, ScratchStatus::programming_error,
                       "an exhausted backing is programming_error, never a capacity status");
        check_rollback_only(*s, 512);
        rollback_ok(*s);
        Model empty(cmp_numeric_text);
        verify(*s, empty, 512, probes, 1, true, "after the exhausted backing is rolled back");
    }
    {
        Rig rig(512, 256);
        Session s(rig, config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::create_empty));
        require_status(s->status(), ScratchStatus::ok, "open");
        begin_ok(*s);
        const Text key = key_u64(2);
        ScratchSpanSource key_source(bytes_of(key));
        FailingSource value_source(300);
        ScratchHandle handle = 0xBAD;
        require_status(s->put(key_source, value_source, handle), ScratchStatus::programming_error,
                       "a source that cannot deliver its bytes is programming_error");
        require(handle == 0xBAD, "a refused put leaves its output");
        require(value_source.reads >= 1, "the store really asked the source for its bytes");
        check_rollback_only(*s, 512);
        rollback_ok(*s);
        Model empty(cmp_numeric_text);
        verify(*s, empty, 512, probes, 1, true, "after the failing source is rolled back");
    }
    {
        Rig rig(512, 16);
        const ScratchStoreConfig config =
            config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::create_empty);
        const std::size_t refused_before = global_allocation::refused;
        global_allocation::refusing = true;
        ScratchStore store(config, &rig.backing);
        global_allocation::refusing = false;
        require(global_allocation::refused > refused_before, "the workspace allocation was really refused");
        expect_unusable(store, ScratchStatus::programming_error, "no memory for the workspace");
        require(rig.counters.reads == 0 && rig.counters.writes == 0 && rig.counters.syncs == 0,
                "a store without a workspace performs no backing I/O");
    }
}

// An extent-growth cap, independent of the store and of write traffic. Once
// armed, overwrites below the existing extent remain serviceable, including
// rollback writes after a refused growth attempt.
class CappedBacking final : public ScratchBacking {
public:
    CappedBacking(FixedMemoryScratchBacking& inner, ScratchIoObserver observer)
        : ScratchBacking(inner.kind(), inner.page_size(), observer), inner_(inner) {}
    std::uint64_t page_count() const noexcept override { return inner_.page_count(); }
    void start_run(std::uint64_t growth_limit_bytes) {
        initial_extent_ = page_count();
        growth_limit_bytes_ = growth_limit_bytes;
    }
    std::uint64_t cap_refusals() const { return cap_refusals_; }

protected:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer destination) const noexcept override {
        return inner_.read_page(id, destination);
    }
    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes source) noexcept override {
        if (id >= page_count() && id + 1 - initial_extent_ > growth_limit_bytes_ / page_size()) {
            ++cap_refusals_;
            return ScratchStatus::io_error;
        }
        return inner_.write_page(id, source);
    }
    ScratchStatus do_sync(ScratchDurability durability) noexcept override {
        return inner_.sync(durability);
    }

private:
    FixedMemoryScratchBacking& inner_;
    std::uint64_t initial_extent_ = 0;
    std::uint64_t growth_limit_bytes_ = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t cap_refusals_ = 0;
};

void extent_cap_reports_io_error_without_partial_state() {
    enum class Operation { put, erase, commit };
    constexpr std::size_t page_size = 512;
    const std::vector<Text> probes = numeric_probes();
    for (const Operation operation : {Operation::put, Operation::erase, Operation::commit}) {
        std::vector<std::byte> storage(page_size * 2048);
        FixedMemoryScratchBacking memory(ScratchBuffer{storage.data(), storage.size()}, page_size);
        Counters counters;
        CappedBacking backing(memory, counters.observer());
        Model committed(cmp_numeric_text);
        {
            // A larger cache defers new-page writes until commit in that case.
            ScratchStore store(config_for(page_size, operation == Operation::commit ? 64 : 2,
                                          1, 1, order_numeric(), ScratchOpenMode::create_empty), &backing);
            require_status(store.status(), ScratchStatus::ok, "open a capped store");
            begin_ok(store);
            if (operation != Operation::commit) {
                for (std::uint64_t k = 0; k < 24; ++k)
                    put_ok(store, committed, key_u64(k), make_value(k, 0, 90));
            }
            commit_ok(store);
            verify(store, committed, page_size, probes, 1, true, "before the capped transaction");

            const std::uint64_t initial_extent = backing.page_count();
            backing.start_run(page_size); // permit one page of growth, then refuse
            begin_ok(store);
            ScratchStatus failure = ScratchStatus::ok;
            if (operation == Operation::put) {
                for (std::uint64_t k = 100; k < 228 && failure == ScratchStatus::ok; ++k) {
                    ScratchHandle handle = 0xBAD;
                    failure = put_text(store, key_u64(k), make_value(k, 1, page_size * 5), handle);
                    if (failure != ScratchStatus::ok)
                        require(handle == 0xBAD, "a capped put leaves its output unchanged");
                }
                require_status(failure, ScratchStatus::io_error, "put preserves an extent-cap io_error");
            } else if (operation == Operation::erase) {
                for (const Row& row : committed.rows) {
                    failure = store.erase(row.handle);
                    if (failure != ScratchStatus::ok) break;
                }
                require_status(failure, ScratchStatus::io_error, "erase preserves an extent-cap io_error");
            } else {
                Model pending = committed;
                for (std::uint64_t k = 0; k < 3; ++k)
                    put_ok(store, pending, key_u64(k), make_value(k, 1, 90));
                failure = store.commit(ScratchDurability::buffered);
                require_status(failure, ScratchStatus::io_error, "commit preserves an extent-cap io_error");
            }
            require(backing.cap_refusals() == 1, "the backing reached the cap and the store did not retry");
            require(backing.page_count() == initial_extent + 1, "one growth write landed before refusal");
            check_rollback_only(store, page_size);
            const std::uint64_t writes_before_rollback = counters.writes;
            rollback_ok(store); // leave the cap armed: rollback must not need growth
            require(!store.transaction_active(), "rollback ends the capped transaction");
            if (operation != Operation::commit)
                require(counters.writes > writes_before_rollback, "rollback restored journaled pages");
            require(backing.cap_refusals() == 1, "rollback overwrites are still serviceable at the cap");
            verify(store, committed, page_size, probes, 1, true, "the cap left no partial logical state");
        }
        ScratchStore reopened(config_for(page_size, 4, 1, 1, order_numeric(), ScratchOpenMode::reopen), &backing);
        require_status(reopened.status(), ScratchStatus::ok, "reopen the same live capped backing");
        verify(reopened, committed, page_size, probes, 1, true, "the cap published no partial state");
    }
}

void geometry_and_open_validation() {
    struct Bad {
        std::size_t page_count;
        std::size_t page_size;
        std::size_t journal_slots;
        std::size_t index_slots;
        bool has_id;
        bool has_compare;
        const char* why;
    };
    const std::array<Bad, 14> table{{
        {0, 512, 1, 1, true, true, "zero cache pages"},
        {1, 512, 1, 1, true, true, "one cache page"},
        {2, 256, 1, 1, true, true, "page size below the floor"},
        {2, 511, 1, 1, true, true, "page size one below the floor"},
        {2, 513, 1, 1, true, true, "page size not a power of two"},
        {2, 768, 1, 1, true, true, "page size 768"},
        {2, 1000, 1, 1, true, true, "page size 1000"},
        {2, 512, 0, 1, true, true, "no journal slots"},
        {2, 512, 1, 0, true, true, "no index slots"},
        {2, 512, 1, 1, false, true, "ordering id zero"},
        {2, 512, 1, 1, true, false, "no comparator"},
        {0, 0, 0, 0, false, false, "everything zero"},
        {1, 100, 1, 1, true, true, "page size 100"},
        {2, 512, 1, 1, true, true, "page size differs from the backing's"},
    }};
    for (std::size_t i = 0; i < table.size(); ++i) {
        const Bad& bad = table[i];
        const std::size_t backing_size = i == table.size() - 1 ? 1024 : (bad.page_size >= 100 ? bad.page_size : 512);
        Rig rig(backing_size, 16);
        ScratchKeyOrder order = order_numeric();
        if (!bad.has_id) order.id = 0;
        if (!bad.has_compare) order.compare = nullptr;
        ScratchStore store(config_for(bad.page_size, bad.page_count, bad.journal_slots, bad.index_slots, order,
                                      ScratchOpenMode::create_empty),
                           &rig.backing);
        expect_unusable(store, ScratchStatus::programming_error, bad.why);
        require(rig.counters.reads == 0 && rig.counters.writes == 0 && rig.counters.syncs == 0,
                std::string(bad.why) + ": a refused geometry performs no I/O");
    }
    {
        Rig rig(512, 16);
        ScratchStore store(config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::create_empty), nullptr);
        expect_unusable(store, ScratchStatus::programming_error, "no backing");
    }
    {
        Rig rig(512, 16);
        ScratchStore store(config_for(512, 2, 1, 1, order_numeric(), ScratchOpenMode::create_empty), &rig.backing);
        require_status(store.status(), ScratchStatus::ok, "the smallest supported geometry opens");
        Rig big(1024, 64);
        ScratchStore other(config_for(1024, 3, 2, 2, order_numeric(), ScratchOpenMode::create_empty), &big.backing);
        require_status(other.status(), ScratchStatus::ok, "a larger geometry opens");
        Rig wide(4096, 64);
        ScratchStore wider(config_for(4096, 8, 4, 4, order_numeric(), ScratchOpenMode::create_empty), &wide.backing);
        require_status(wider.status(), ScratchStatus::ok, "a 4096-byte geometry opens");
    }
}

void differential_random_walk() {
    struct Geometry {
        std::size_t page_size;
        std::size_t page_count;
        std::size_t journal_slots;
        std::size_t index_slots;
        std::size_t pages;
    };
    const std::array<Geometry, 4> geometries{{
        {512, 2, 1, 1, 8000},
        {512, 3, 2, 2, 8000},
        {1024, 2, 1, 1, 4000},
        {4096, 5, 3, 1, 1200},
    }};
    const std::vector<Text> probes = numeric_probes();
    for (const Geometry& g : geometries) {
        const std::string label = "geometry " + std::to_string(g.page_size) + "/" + std::to_string(g.page_count);
        Rig rig(g.page_size, g.pages);
        Session s(rig, config_for(g.page_size, g.page_count, g.journal_slots, g.index_slots, order_numeric(),
                                  ScratchOpenMode::create_empty));
        require_status(s->status(), ScratchStatus::ok, label + ": open");
        Model model(cmp_numeric_text);
        Rng rng(g.page_size * 31 + g.page_count);
        std::vector<std::byte> page(g.page_size);
        for (std::size_t txn = 0; txn < 120; ++txn) {
            begin_ok(*s);
            Model pending = model;
            require_status(random_ops(*s, pending, rng, 1 + static_cast<std::size_t>(rng.below(24))), ScratchStatus::ok,
                           label + ": random edits");
            if (txn % 10 == 0) verify(*s, pending, g.page_size, probes, 1, false, label + ": pending state");
            if (rng.below(100) < 30) {
                rollback_ok(*s);
            } else {
                commit_ok(*s);
                model = pending;
            }
            std::uint64_t digest = 0, state = 0;
            require_status(s->ordered_digest(ScratchBuffer{page.data(), page.size()}, digest), ScratchStatus::ok,
                           label + ": digest");
            require_status(s->state_digest(ScratchBuffer{page.data(), page.size()}, state), ScratchStatus::ok,
                           label + ": state digest");
            require(s->record_count() == model.rows.size() && digest == expected_ordered_digest(model)
                    && state == expected_state_digest(model, 1), label + ": digests after every transaction");
            if (txn % 25 == 24) verify(*s, model, g.page_size, probes, 1, true, label + ": full check");
            if (txn % 40 == 39) {
                s.close();
                s.open(config_for(g.page_size, 2 + txn % 5, 1 + txn % 3, 1 + txn % 2, order_numeric(),
                                  ScratchOpenMode::reopen));
                require_status(s->status(), ScratchStatus::ok, label + ": mid-walk reopen");
                verify(*s, model, g.page_size, probes, 1, true, label + ": after mid-walk reopen");
            }
        }
    }
}
} // namespace

int main() {
    try {
        oracle_matches_published_vectors();
        extent_cap_reports_io_error_without_partial_state();
        ascending_after_insert_and_erase();
        handles_are_successive_stable_and_never_recycled();
        forced_eviction_then_reread_by_handle();
        reentrant_reads_during_iteration();
        comparator_cannot_mutate_the_store();
        rollback_after_partial_writes();
        rollback_survives_torn_writes_at_every_phase();
        failed_commit_publishes_nothing();
        durability_failure_and_transaction_state_machine();
        commit_then_reopen_and_abandon();
        digest_is_throughput_neutral();
        digest_covers_every_stored_byte();
        no_heap_after_construction();
        space_is_reused_under_churn();
        rollback_does_not_leak_space();
        handle_table_space_follows_the_live_range();
        rollback_can_be_retried_after_a_refused_write();
        durable_commit_publishes_between_two_syncs();
        keys_and_values_larger_than_the_cache();
        equal_keys_upsert_and_key_bytes();
        misuse_and_error_surface();
        named_failure_causes_are_programming_errors();
        geometry_and_open_validation();
        differential_random_walk();
        std::puts("test_scratch_store: order, handles, eviction, reentrancy, rollback, reopen, digests, no heap");
        return 0;
    } catch (const std::exception& error) {
        global_allocation::refusing = false;
        std::fprintf(stderr, "test_scratch_store: %s\n", error.what());
        return 1;
    }
}
