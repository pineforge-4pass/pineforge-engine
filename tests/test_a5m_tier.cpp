// The matrix Runtime's resident tier (detail/matrix_store.hpp) under an
// explicit owner that opens it inside its own transaction, over a Store that
// counts every call it forwards. Rows:
//  - store path: a Runtime whose owner never opens the tier makes exactly the
//    Store path's calls: a get one read, a set one read and one write, a
//    transpose one read, one create and one write, an eigenvalue read two reads.
//  - steady state: with the tier open, a corpus-shaped bar (a 24x7 boolean
//    mask with at most one set, two transposes and 169 gets; a 2x2 covariance
//    with four sets and two eigenvalue reads) makes no Store call after the
//    first bar, and its gets, sets, rows and columns allocate nothing. Its
//    spectrum is the heap path's. flush_tier() then creates exactly the two
//    matrices made in the tier and still alive, writes the four changed ones
//    once each, and leaves the Store holding every matrix's exact bytes and the
//    tier empty and closed.
//  - drop: drop_tier() and the owner's rollback leave the Store and every
//    matrix made before the transaction as they were; a matrix made in the
//    tier during it reads as a dead Store object.
//  - refused write: in a transaction made rollback-only, a write to a
//    resident matrix is refused (RollbackOnly) where the Store would refuse
//    it, and changes nothing; reads still answer.
//  - eviction: three times as many live matrices as slots: the tier holds at
//    most kResidentSlots objects and kResidentBytes, slots are written back or
//    created in the Store as they are reused, and every cell reads back exact
//    during the transaction and from the Store after it, each matrix under its
//    own live id.
//  - borrow and identity: identity() of a matrix made in the tier is a live
//    Store id holding its bytes, and its hash folds that id and those bytes; a
//    borrow of a resident object reads, and writes, the tier's newer bytes.
//  - detach and release: a detached matrix leaves the tier with its bytes in
//    the Store; a borrow of its id reads the Store without loading it into the
//    tier; once its new owner releases the id with Store::release, the borrow
//    reads a dead id and the flush still succeeds.
//  - restore: a matrix restored from its snapshot inside the transaction reads
//    the snapshot's cells, before and after the flush.
//  - unflushed call: a run's call (RunRuntime, RunScope) that ends without its
//    flush keeps none of its resident changes: the call rolls back and its
//    trace is one rollback-only cleanup.
//
// Every expected cell comes from a model built here; every expected Store byte
// from an image built here and read from the Store under the counter.
//
// FAIL-BEFORE. Against the previous header closure this unit does not compile:
// Runtime has no tier_open() (the first diagnostic, in the store path row),
// tier_stats(), open_tier(), flush_tier(), drop_tier() or tier_changed(),
// RunRuntime has no flush() or open_over(), and there is no kResidentSlots.
#include "global_allocation_replacement.hpp"
#include "a5m_store_fixture.hpp"

#include <pineforge/detail/matrix_store.hpp>
#include <pineforge/generic_matrix.hpp>
#include <pineforge/matrix.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace ms = pineforge::detail::matrix_store;
namespace o1 = pineforge::detail::o1;
using a5m_test::require;
using Mask = pineforge::GenericMatrix<bool>;
using Numeric = pineforge::NumericMatrix;

// Store calls by kind.
struct Calls {
    std::uint64_t create = 0, release = 0, alive = 0, size = 0, resize = 0, read = 0, write = 0;
    std::uint64_t total() const { return create + release + alive + size + resize + read + write; }
};

Calls operator-(const Calls& a, const Calls& b) {
    Calls d;
    d.create = a.create - b.create;
    d.release = a.release - b.release;
    d.alive = a.alive - b.alive;
    d.size = a.size - b.size;
    d.resize = a.resize - b.resize;
    d.read = a.read - b.read;
    d.write = a.write - b.write;
    return d;
}

Calls calls(std::uint64_t create, std::uint64_t read, std::uint64_t write) {
    Calls c;
    c.create = create;
    c.read = read;
    c.write = write;
    return c;
}

// A Store seen through a call counter: every call is forwarded unchanged and
// every object call is counted; both transaction hooks are forwarded.
class CallCounter final : public o1::Store {
public:
    explicit CallCounter(o1::Store& inner) : inner_(inner) {}
    o1::Id create(std::uint64_t bytes, std::uint64_t align) override {
        ++calls_.create;
        return inner_.create(bytes, align);
    }
    void release(o1::Id id) override {
        ++calls_.release;
        inner_.release(id);
    }
    bool alive(o1::Id id) const override {
        ++calls_.alive;
        return inner_.alive(id);
    }
    std::uint64_t size(o1::Id id) const override {
        ++calls_.size;
        return inner_.size(id);
    }
    bool resize(o1::Id id, std::uint64_t bytes) override {
        ++calls_.resize;
        return inner_.resize(id, bytes);
    }
    std::size_t read(o1::Id id, std::uint64_t off, void* dst, std::size_t n) const override {
        ++calls_.read;
        return inner_.read(id, off, dst, n);
    }
    std::size_t write(o1::Id id, std::uint64_t off, const void* src, std::size_t n) override {
        ++calls_.write;
        return inner_.write(id, off, src, n);
    }
    std::unique_ptr<o1::Tx> begin() override { return inner_.begin(); }
    bool join_active_tx() override { return inner_.join_active_tx(); }
    void abandon_joined_tx() noexcept override { inner_.abandon_joined_tx(); }
    std::unique_ptr<o1::Tx> begin_cleanup() override { return inner_.begin_cleanup(); }
    void note_cleanup_skipped(o1::CleanupSkip reason) noexcept override {
        inner_.note_cleanup_skipped(reason);
    }
    o1::Stats stats() const override { return inner_.stats(); }
    Calls counted() const noexcept { return calls_; }

private:
    o1::Store& inner_;
    mutable Calls calls_;
};

// A Runtime over the counter, current for the row: matrices made here live in
// the fixture's Store.
struct Fixture {
    a5m_test::StoreFixture fixture;
    CallCounter counter{*fixture.store};
    ms::Runtime runtime{counter};
    ms::ScopedRuntime scope{runtime};
    o1::Store& real() { return *fixture.store; }
};

using MaskModel = std::array<char, 24 * 7>;

std::vector<char> transposed(const MaskModel& model) {
    std::vector<char> out(model.size());
    for (int h = 0; h < 24; ++h)
        for (int d = 0; d < 7; ++d)
            out[static_cast<std::size_t>(d * 24 + h)] = model[static_cast<std::size_t>(h * 7 + d)];
    return out;
}

// A matrix object's bytes as the Store holds them: the descriptor (rows,
// columns, record width, zero), then the row-major records.
template <class Record>
std::vector<unsigned char> image(std::uint64_t rows, std::uint64_t columns,
                                 const std::vector<Record>& records) {
    const std::uint64_t descriptor[4] = {rows, columns, sizeof(Record), 0};
    std::vector<unsigned char> bytes(sizeof descriptor + records.size() * sizeof(Record));
    std::memcpy(bytes.data(), descriptor, sizeof descriptor);
    if (!records.empty())
        std::memcpy(bytes.data() + sizeof descriptor, records.data(),
                    records.size() * sizeof(Record));
    return bytes;
}

std::vector<unsigned char> stored(const o1::Store& store, o1::Id id) {
    std::vector<unsigned char> bytes(static_cast<std::size_t>(store.size(id)));
    require(store.read(id, 0, bytes.data(), bytes.size()) == bytes.size(),
            "short Store read of a matrix object");
    return bytes;
}

std::uint64_t fold(std::uint64_t hash, const unsigned char* bytes, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    return hash;
}

void store_path() {
    Fixture f;
    require(!f.runtime.tier_open() && f.runtime.tier_stats().objects == 0,
            "an explicit Runtime opened a tier its owner never asked for");
    auto mask = Mask::new_(24, 7, false);
    auto cov = Numeric::new_(3, 3, 0.5);
    const auto expect = [&](const char* what, auto&& operation, const Calls& want) {
        const auto before = f.counter.counted();
        operation();
        const auto got = f.counter.counted() - before;
        if (got.create != want.create || got.release != want.release ||
            got.alive != want.alive || got.size != want.size || got.resize != want.resize ||
            got.read != want.read || got.write != want.write) {
            std::fprintf(stderr,
                         "A5M tier store path %s: create=%llu release=%llu alive=%llu "
                         "size=%llu resize=%llu read=%llu write=%llu\n",
                         what, static_cast<unsigned long long>(got.create),
                         static_cast<unsigned long long>(got.release),
                         static_cast<unsigned long long>(got.alive),
                         static_cast<unsigned long long>(got.size),
                         static_cast<unsigned long long>(got.resize),
                         static_cast<unsigned long long>(got.read),
                         static_cast<unsigned long long>(got.write));
            require(false, "an explicit Runtime's operation left the Store path's call counts");
        }
    };
    Mask flipped;
    expect("mask get", [&] { (void)mask.get(3, 4); }, calls(0, 1, 0));
    expect("mask set", [&] { mask.set(3, 4, true); }, calls(0, 1, 1));
    expect("mask transpose", [&] { flipped = mask.transpose(); }, calls(1, 1, 1));
    expect("numeric get", [&] { (void)cov.get(2, 1); }, calls(0, 1, 0));
    expect("numeric set", [&] { cov.set(2, 1, -4.0); }, calls(0, 1, 1));
    expect("numeric eigenvalues", [&] { (void)cov.eigenvalues(); }, calls(0, 2, 0));
    require(flipped.get(4, 3) && cov.get(2, 1) == -4.0, "the Store path read a wrong cell");
    require(f.runtime.tier_stats().loaded == 0 && f.runtime.tier_stats().made == 0,
            "an explicit Runtime held an object in a tier");
}

void steady_state() {
    Fixture f;
    auto owner = f.counter.begin();
    f.runtime.open_tier();
    require(f.runtime.tier_open(), "open_tier() left the tier closed");
    auto mask = Mask::new_(24, 7, false);
    auto mT = Mask::new_(7, 24, false);
    auto mTT = Mask::new_(24, 7, false);
    auto cov = Numeric::new_(2, 2, 0.0);
    MaskModel model{};
    constexpr int kBars = 96;
    double spread = 0.0;
    for (int bar = 0; bar < kBars; ++bar) {
        const auto before = f.counter.counted();
        const int h = bar % 24, d = (bar / 24) % 7;
        auto allocations = global_allocation::allocations;
        if (bar % 3 != 2) {
            mask.set(h, d, true);
            model[static_cast<std::size_t>(h * 7 + d)] = 1;
        }
        require(global_allocation::allocations == allocations, "a resident set allocated");
        mT = mask.transpose();
        mTT = mT.transpose();
        allocations = global_allocation::allocations;
        bool exact = mTT.rows() == 24 && mTT.columns() == 7 &&
                     mTT.get(h, d) == (model[static_cast<std::size_t>(h * 7 + d)] != 0);
        for (int i = 0; i < 24; ++i)
            for (int j = 0; j < 7; ++j)
                exact = exact && mTT.get(i, j) == (model[static_cast<std::size_t>(i * 7 + j)] != 0);
        spread = 0.125 * bar;
        cov.set(0, 0, 1.0 + spread);
        cov.set(0, 1, spread);
        cov.set(1, 0, spread);
        cov.set(1, 1, 1.0 + spread);
        require(global_allocation::allocations == allocations,
                "resident gets, sets, rows or columns allocated");
        require(exact, "a resident get read a wrong cell");
        const auto first = cov.eigenvalues();
        const auto second = cov.eigenvalues();
        std::vector<double> heap_spectrum;
        {
            const ms::ScopedRuntime native(nullptr);
            auto heap = Numeric::new_(2, 2, 0.0);
            heap.set(0, 0, 1.0 + spread);
            heap.set(0, 1, spread);
            heap.set(1, 0, spread);
            heap.set(1, 1, 1.0 + spread);
            heap_spectrum = heap.eigenvalues();
        }
        require(first.size() == 2 && first == second && first == heap_spectrum,
                "the resident spectrum differs from the heap path's");
        if (bar > 0)
            require((f.counter.counted() - before).total() == 0,
                    "a bar after the first made a Store call");
    }
    require(f.runtime.tier_changed(), "the tier held no change before its flush");
    const auto before = f.counter.counted();
    f.runtime.flush_tier();
    const auto flushed = f.counter.counted() - before;
    owner->commit();
    const auto tier = f.runtime.tier_stats();
    require(!f.runtime.tier_open() && tier.objects == 0 && tier.bytes == 0,
            "flush_tier() left the tier open or holding objects");
    require(flushed.create == 2 && flushed.write == 4 && flushed.read == 0 &&
                flushed.release == 0 && flushed.resize == 0,
            "flush_tier() did not create the two matrices made in the tier and write the four "
            "changed ones once each");
    require(f.real().stats().objects_live == 4,
            "the Store does not hold exactly the four live matrices");
    const std::vector<char> rows(model.begin(), model.end());
    require(stored(f.real(), mask.external_identity()) == image(24, 7, rows) &&
                stored(f.real(), mTT.external_identity()) == image(24, 7, rows) &&
                stored(f.real(), mT.external_identity()) == image(7, 24, transposed(model)),
            "the flushed Store does not hold a mask's exact bytes");
    require(stored(f.real(), cov.external_identity()) ==
                image(2, 2, std::vector<double>{1.0 + spread, spread, spread, 1.0 + spread}),
            "the flushed Store does not hold the covariance's exact bytes");
}

void drop() {
    Fixture f;
    auto mask = Mask::new_(24, 7, false);
    mask.set(1, 2, true);
    const auto hash = mask.external_hash();
    const auto live = f.real().stats();
    Mask made;
    {
        auto owner = f.counter.begin();
        f.runtime.open_tier();
        mask.set(3, 4, true);
        made = mask.transpose();
        require(mask.get(3, 4) && made.get(4, 3) && made.get(2, 1),
                "a write inside the transaction was not read back");
        require(f.runtime.tier_changed(), "the tier held no change to drop");
        f.runtime.drop_tier();
        owner->rollback();
    }
    require(!f.runtime.tier_open() && f.runtime.tier_stats().objects == 0,
            "drop_tier() left the tier open or holding objects");
    require(!mask.get(3, 4) && mask.get(1, 2) && mask.external_hash() == hash,
            "the rollback kept a resident write");
    const auto after = f.real().stats();
    require(after.objects_live == live.objects_live && after.bytes_live == live.bytes_live,
            "the rollback kept a Store object made in the dropped tier");
    bool dead = false;
    try {
        (void)made.get(0, 0);
    } catch (const std::invalid_argument& error) {
        dead = std::string(error.what()) == "o1 store: dead id";
    }
    require(dead, "a matrix made in a dropped tier did not read as a dead Store object");
}

void refused_write() {
    Fixture f;
    auto mask = Mask::new_(24, 7, false);
    auto owner = f.counter.begin();
    f.runtime.open_tier();
    mask.set(0, 0, true);
    require(f.runtime.tier_stats().objects == 1, "the written matrix is not resident");
    {
        // A host scope that joins the owner's transaction and rolls back
        // leaves it rollback-only (there is no savepoint).
        auto joined = f.counter.begin();
        joined->rollback();
    }
    bool refused = false;
    try {
        mask.set(1, 1, true);
    } catch (const o1::RollbackOnly&) {
        refused = true;
    }
    require(refused, "a resident write in a rollback-only transaction was admitted");
    require(mask.get(0, 0) && !mask.get(1, 1), "a refused resident write changed the matrix");
    f.runtime.drop_tier();
    owner->rollback();
    require(!mask.get(0, 0), "the rollback kept a resident write");
}

void eviction() {
    Fixture f;
    constexpr int kMatrices = 3 * static_cast<int>(ms::kResidentSlots);
    constexpr int kRounds = 3;
    auto owner = f.counter.begin();
    f.runtime.open_tier();
    const auto bounded = [&] {
        const auto tier = f.runtime.tier_stats();
        require(tier.objects <= ms::kResidentSlots && tier.bytes <= ms::kResidentBytes,
                "the tier held more than its fixed budget");
    };
    auto base = Numeric::new_(2, 2, 0.0);
    std::vector<Numeric> copies;
    for (int i = 0; i < kMatrices; ++i) {
        base.set(0, 0, i);
        base.set(1, 1, -i);
        copies.push_back(base.copy());
        bounded();
    }
    for (int round = 0; round < kRounds; ++round)
        for (int i = 0; i < kMatrices; ++i) {
            auto& copy = copies[static_cast<std::size_t>(i)];
            require(copy.get(0, 0) == i && copy.get(1, 1) == -i && copy.get(1, 0) == 0.0 &&
                        copy.get(0, 1) == (round ? (round - 1) * 100.0 + i : 0.0),
                    "a matrix read back a wrong cell after its slot was reused");
            copy.set(0, 1, round * 100.0 + i);
            bounded();
        }
    const auto tier = f.runtime.tier_stats();
    require(tier.evicted > 0 && tier.stored > 0 && tier.written > 0,
            "no slot was written back or created in the Store as it was reused");
    f.runtime.flush_tier();
    owner->commit();
    require(f.real().stats().objects_live == static_cast<std::uint64_t>(kMatrices) + 1,
            "the Store does not hold exactly the live matrices");
    std::vector<o1::Id> ids;
    for (int i = 0; i < kMatrices; ++i) {
        const auto id = copies[static_cast<std::size_t>(i)].external_identity();
        require(id != o1::kNullId && f.real().alive(id), "a matrix has no live Store object");
        for (const auto other : ids) require(other != id, "two matrices share one Store id");
        ids.push_back(id);
        const double last = (kRounds - 1) * 100.0 + i;
        require(stored(f.real(), id) ==
                    image(2, 2, std::vector<double>{static_cast<double>(i), last, 0.0,
                                                    static_cast<double>(-i)}),
                "the Store does not hold a reused slot's exact bytes");
    }
}

void borrow_and_identity() {
    Fixture f;
    auto owner = f.counter.begin();
    f.runtime.open_tier();
    auto mask = Mask::new_(24, 7, false);
    auto flipped = mask.transpose();
    flipped.set(2, 5, true);
    const auto creates = f.counter.counted().create;
    const auto id = flipped.external_identity();
    require(id != o1::kNullId && f.real().alive(id) && f.counter.counted().create == creates + 1,
            "identity() of a matrix made in the tier is not a new live Store id");
    require(flipped.get(2, 5) && !flipped.get(5, 2), "identity() lost the matrix's cells");
    std::vector<char> cells(24 * 7, 0);
    cells[2 * 24 + 5] = 1;
    const auto bytes = stored(f.real(), id);
    require(bytes == image(7, 24, cells), "identity() left the Store without the matrix's bytes");
    std::uint64_t hash = UINT64_C(1469598103934665603);
    for (unsigned int i = 0; i < 8; ++i) {
        const auto byte = static_cast<unsigned char>(id >> (8 * i));
        hash = fold(hash, &byte, 1);
    }
    hash = fold(hash, bytes.data(), bytes.size());
    require(flipped.external_hash() == hash, "the hash does not fold the matrix's id and bytes");
    auto view = Mask::borrow_external(f.runtime, id);
    flipped.set(0, 0, true);
    require(view.get(0, 0), "a borrow read the Store's older bytes");
    view.set(6, 23, true);
    require(flipped.get(6, 23), "the owner missed a write made through a borrow");
    mask.set(4, 4, true);
    auto mask_view = Mask::borrow_external(f.runtime, mask.external_identity());
    require(mask_view.get(4, 4), "a borrow of a resident matrix read its older bytes");
    f.runtime.flush_tier();
    owner->commit();
    require(view.get(0, 0) && view.get(6, 23) && view.get(2, 5) && mask_view.get(4, 4),
            "the flush lost a write made through an owner or a borrow");
}

void detach_and_release() {
    Fixture f;
    auto owner = f.counter.begin();
    f.runtime.open_tier();
    auto mask = Mask::new_(24, 7, false);
    auto flipped = mask.transpose();
    flipped.set(1, 1, true);
    const auto id = flipped.detach_external();
    std::vector<char> cells(24 * 7, 0);
    cells[1 * 24 + 1] = 1;
    require(id != o1::kNullId && f.real().alive(id) &&
                stored(f.real(), id) == image(7, 24, cells),
            "detach() left the Store without the matrix's bytes");
    require(f.runtime.tier_stats().objects == 1, "a detached matrix stayed in the tier");
    auto view = Mask::borrow_external(f.runtime, id);
    require(view.get(1, 1) && !view.get(0, 0), "a borrow of a detached matrix read a wrong cell");
    require(f.runtime.tier_stats().objects == 1, "a borrow loaded its object into the tier");
    // The new owner releases the id with the Store, which the tier never sees.
    f.counter.release(id);
    bool dead = false;
    try {
        (void)view.get(1, 1);
    } catch (const std::invalid_argument& error) {
        dead = std::string(error.what()) == "o1 store: dead id";
    }
    require(dead, "a borrow of a released id read a stale copy");
    f.runtime.flush_tier();
    owner->commit();
    require(f.real().stats().objects_live == 1 && !f.real().alive(id),
            "the flush revived a released matrix or lost the live one");
}

void restore() {
    Fixture f;
    auto owner = f.counter.begin();
    f.runtime.open_tier();
    auto cov = Numeric::new_(3, 3, 0.25);
    cov.set(1, 2, 7.5);
    const auto snapshot = cov.snapshot();
    cov.set(1, 2, -1.0);
    cov.add_row(0, {1.0, 2.0, 3.0});
    cov.restore(snapshot);
    const auto exact = [&] {
        return cov.rows() == 3 && cov.columns() == 3 && cov.get(1, 2) == 7.5 &&
               cov.get(0, 0) == 0.25 && cov.get(2, 2) == 0.25;
    };
    require(exact(), "a restore inside the transaction read a stale slot");
    f.runtime.flush_tier();
    owner->commit();
    require(exact(), "a restored matrix lost its cells in the flush");
    require(f.real().stats().objects_live == 2,
            "the Store does not hold exactly the matrix and its snapshot");
}

void unflushed_call() {
    ms::RunRuntime run;
    run.open_over(std::make_unique<a5m_test::FileBacking>());
    Numeric kept;
    {
        const ms::RunScope call(run);
        kept = Numeric::new_(2, 2, 1.0);
        require(run.opened() != nullptr && run.opened()->tier_open(),
                "a run's call did not open its Runtime's tier");
        run.flush();
    }
    {
        const ms::RunScope call(run);
        kept.set(0, 0, 5.0);
        const auto made = kept.transpose();
        require(made.get(0, 0) == 5.0 && run.opened()->tier_changed(),
                "the call's change was not held in the tier");
    }
    const auto tier = run.opened()->tier_stats();
    require(!run.opened()->tier_open() && tier.objects == 0,
            "a call that ended without its flush left the tier open or holding objects");
    require(kept.get(0, 0) == 1.0 && kept.get(1, 1) == 1.0,
            "a call that ended without its flush kept a resident change");
    const auto stats = run.opened()->store().stats();
    require(stats.cleanup_skipped_rollback_only == 1 && stats.cleanup_failed == 0 &&
                stats.objects_live == 1,
            "a call that ended without its flush did not roll back");
}

} // namespace

int main() {
    bool ok = true;
    const auto row = [&ok](const char* name, void (*body)()) {
        try {
            body();
        } catch (const std::exception& error) {
            ok = false;
            std::fprintf(stderr, "A5M tier %s: %s\n", name, error.what());
        }
    };
    row("store path", store_path);
    row("steady state", steady_state);
    row("drop", drop);
    row("refused write", refused_write);
    row("eviction", eviction);
    row("borrow and identity", borrow_and_identity);
    row("detach and release", detach_and_release);
    row("restore", restore);
    row("unflushed call", unflushed_call);
    if (ok) std::puts("A5M tier: every row reached");
    return ok ? 0 : 1;
}
