// s4e-ei2 round 1. Edit-only: no executed result is claimed by this file.
//
// COST DERIVATION (before measurement, not a fit):
// E2 Q2 gives A(L)=40L+64 direct loads, W(L)=24L+64 keyed puts,
// L=ceil(1.45*log2(U+2))+2. A <=48-page allowance covers a <=512-byte
// point/value read (handle radix height <=12, 56-byte cells).
// At 512 bytes the store's ordered leaf/interior capacities are 58/29.
// descend does <=6 comparisons per level. A <=64-byte key comparison needs
// <=48 references (handle lookup plus two cells, including offset reads).
// For keyed put/erase, allow 4096*(H+1) page reads: 6*48 for comparisons,
// 58*16 for bounded split/shift/repair accesses, 12*64 for handle-table
// allocation/pruning, 4*64 for key/value cell replacement, and 3*512 for
// journal/touch/free-list/commit accesses, rounded UP to 4096 per level.
// This includes commit; handle-based put is neither assumed nor implemented.
// Here R<=2U+16 (one cohort and at most one detached generation). Taking only
// binary branching gives ordered height H<=m+3, m=ceil(log2(max(U,2))).
// Add 128 direct loads and 32 keyed operations for descriptors, directory,
// pre-search, two GC work items, and transaction metadata:
//   reads <=48*(40L+192)+4096*(m+4)*(24L+96)+4096.
// For these U>=17, m>=5 and L<=3m. The expression is <2^20*m*m.
// Thus c=2^20 below is algebraic and intentionally conservative. A separate
// growth gate uses the log^2 ratio plus a fixed 4096-reference metadata
// allowance; this gate detects a linear suffix rewrite even under loose c.
// The bound is for this pinned geometry and record population, not arbitrary
// id lengths, unrelated store populations, EI1's future nodes, or fixture k.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include <pineforge/source/cohort_unit_owner.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
using namespace pineforge::detail;
using Owner = pineforge::source::CohortUnitOwner;
using Failure = pineforge::source::ExposureOwnerFailure;
using Reason = pineforge::source::ExposureOwnerError;
using Model = std::map<std::uint64_t, std::uint64_t>;
constexpr std::size_t page_bytes = 512;
constexpr std::uint64_t cohort = 19;
constexpr std::uint64_t seed = 0x4f31533445493201ULL;
static_assert(sizeof(Owner) <= 512 && sizeof(Owner::const_iterator) <= 256
                  && sizeof(Owner::ReadFacade) <= 32, "fixed owner, iterator and cohort attachments");

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void ok(ScratchStatus status) { require(status == ScratchStatus::ok, "scratch status is ok"); }
std::uint64_t bits(double value) {
    std::uint64_t word = 0;
    std::memcpy(&word, &value, sizeof(word));
    return word;
}
struct Generator {
    std::uint64_t state = seed;
    std::uint64_t next() {
        auto z = (state += std::uint64_t{0x9e3779b97f4a7c15ULL});
        z = (z ^ (z >> 30)) * std::uint64_t{0xbf58476d1ce4e5b9ULL};
        z = (z ^ (z >> 27)) * std::uint64_t{0x94d049bb133111ebULL};
        return z ^ (z >> 31);
    }
};
ScratchStatus lexical(void*, const ScratchByteSource& a, const ScratchByteSource& b,
                      int& order) noexcept {
    std::array<std::byte, 32> left{}, right{};
    const auto common = std::min(a.size(), b.size());
    for (std::uint64_t at = 0; at < common;) {
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(left.size(), common - at));
        auto status = a.read(at, {left.data(), n});
        if (status != ScratchStatus::ok) return status;
        status = b.read(at, {right.data(), n});
        if (status != ScratchStatus::ok) return status;
        const auto compare = std::memcmp(left.data(), right.data(), n);
        if (compare) { order = compare < 0 ? -1 : 1; return ScratchStatus::ok; }
        at += n;
    }
    order = a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
    return ScratchStatus::ok;
}
ScratchStatus reverse_lexical(void* context, const ScratchByteSource& a,
                              const ScratchByteSource& b, int& order) noexcept {
    const auto status = lexical(context, a, b, order);
    if (status == ScratchStatus::ok) order = -order;
    return status;
}

// Test memory is provisioned once, before every measured window. This wrapper
// follows no files. It injects failures in the real borrowed store's backing,
// including journal/publication writes, without replacing any store logic.
class FaultBacking final : public ScratchBacking {
public:
    explicit FaultBacking(ScratchBuffer bytes)
        : ScratchBacking(ScratchBackingKind::test_memory, page_bytes),
          memory_(bytes, page_bytes) {}
    std::uint64_t page_count() const noexcept override { return memory_.page_count(); }
    std::uint64_t writes = 0, fail_write = 0;
    bool fired = false;
    void arm(std::uint64_t offset) { fail_write = writes + offset; fired = false; }
    void disarm() { fail_write = 0; }
private:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer out) const noexcept override {
        return memory_.read_page(id, out);
    }
    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes in) noexcept override {
        ++writes;
        if (fail_write && writes == fail_write) {
            fired = true;
            return ScratchStatus::io_error;
        }
        return memory_.write_page(id, in);
    }
    ScratchStatus do_sync(ScratchDurability durability) noexcept override {
        return memory_.sync(durability);
    }
    FixedMemoryScratchBacking memory_;
};
struct Rig {
    std::vector<std::byte> bytes;
    FaultBacking backing;
    Rig() : bytes(131072 * page_bytes), backing({bytes.data(), bytes.size()}) {}
    ScratchStoreConfig config(ScratchOpenMode mode = ScratchOpenMode::create_empty,
                              bool reverse = false) const {
        return {2, page_bytes, 1, 1,
            {reverse ? 0x45493202ULL : 0x45493201ULL, nullptr,
             reverse ? &reverse_lexical : &lexical}, mode};
    }
};
std::uint64_t digest(const ScratchStore& store) {
    std::array<std::byte, page_bytes> page{};
    std::uint64_t result = 0;
    ok(store.state_digest({page.data(), page.size()}, result));
    return result;
}
void initialize(Owner& owner, ScratchStore& store) {
    ok(store.status());
    require(store.page_size() == page_bytes, "512-byte geometry retained");
    owner.create(store);
    require(owner.ensure_cohort("cohort", cohort), "cohort descriptor created");
}
void compare(const Owner& owner, const Model& model, std::uint64_t who = cohort) {
    const auto units = owner.units(who);
    require(units.size() == model.size() && units.empty() == model.empty(), "exact map size");
    auto actual = units.begin();
    const auto end = units.end();
    std::size_t rank = 0;
    for (auto want = model.begin(); want != model.end(); ++want, ++rank) {
        require(actual != end, "enumeration is not truncated");
        require(actual->first == want->first, "ascending unsigned incarnation enumeration");
        require(actual.raw_bits() == want->second && bits(actual->second) == want->second,
                "FULL enumeration compares raw value bits as well as keys");
        if (model.size() <= 17 || rank == 0 || rank == model.size() / 2 || rank + 1 == model.size()) {
            const auto counted = units.nth(rank);
            require(counted != end && counted->first == want->first
                        && counted.raw_bits() == want->second, "subtree-count selection agrees");
        }
        ++actual;
    }
    require(actual == end && units.nth(model.size()) == end, "no extra or stale unit rows");
}
std::uint64_t choose(const Model& model, std::uint64_t random) {
    require(!model.empty(), "choose a present model row");
    auto it = model.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(random % model.size()));
    return it->first;
}
std::uint64_t absent(const Model& model, std::uint64_t candidate) {
    while (model.find(candidate) != model.end()) ++candidate;
    return candidate;
}
void set_and_compare(Owner& owner, Model& model, std::uint64_t key, std::uint64_t value) {
    owner.set_bits(cohort, key, value);
    model[key] = value;
    compare(owner, model);
}

void cohort_unit_differential() {
    const std::array<std::size_t, 9> sizes{{0, 1, 2, 3, 7, 17, 33, 65, 257}};
    unsigned coverage = 0;
    std::size_t operations = 0;
    for (const auto size : sizes) {
        Rig rig;
        ScratchStore store(rig.config(), &rig.backing);
        Owner owner;
        initialize(owner, store);
        Model model;
        Generator rng;
        rng.state ^= static_cast<std::uint64_t>(size);
        compare(owner, model);
        for (std::size_t i = 0; i < size; ++i) {
            const auto random_key = rng.next();
            const auto raw = rng.next(); // Sequenced before function arguments.
            const auto key = absent(model, random_key);
            require(owner.insert_bits(cohort, key, raw), "arbitrary-key insertion is new");
            model.emplace(key, raw);
            compare(owner, model);
        }
        owner.append_origin(cohort, 42, 100); // Slot persists independently of units.
        compare(owner, model);
        const auto mutations = size <= 17 ? 48U : 12U;
        for (unsigned step = 0; step < mutations; ++step) {
            const auto random_key = rng.next();
            const auto raw = rng.next();
            const auto missing = absent(model, random_key);
            const auto present = model.empty() ? missing : choose(model, random_key);
            const auto which = step % 12;
            coverage |= 1U << which;
            switch (which) {
            case 0:
                require(owner.insert_bits(cohort, missing, raw), "insert absent");
                model.emplace(missing, raw);
                break;
            case 1:
                owner.set_bits(cohort, present, raw);
                model[present] = raw;
                break;
            case 2:
                require(owner.erase(cohort, present) == model.erase(present), "erase by key");
                break;
            case 3:
                if (!model.empty()) {
                    const auto at = model.find(present);
                    const auto next = std::next(at);
                    const auto it = owner.units(cohort).find(present);
                    const auto survivor_handle = next == model.end() ? 0
                        : owner.units(cohort).find(next->first).node_handle();
                    const auto after = owner.erase(cohort, it);
                    require((next == model.end() && after == owner.units(cohort).end())
                                || (next != model.end() && after->first == next->first
                                    && after.raw_bits() == next->second
                                    && after.node_handle() == survivor_handle),
                            "stable-node iterator erase returns original successor");
                    model.erase(at);
                }
                break;
            case 4:
                require(!owner.units(cohort).contains(missing)
                            && owner.units(cohort).count(missing) == 0, "absent const lookup");
                break;
            case 5:
                if (!model.empty()) {
                    require(!owner.insert_bits(cohort, present, raw), "duplicate insert is not replacement");
                    require(bits(owner.units(cohort).at(present)) == model.at(present),
                            "const at returns original raw bits");
                }
                break;
            case 6:
                owner.clear(cohort);
                model.clear();
                require(owner.origin_slot(cohort, 42).count == 1, "unit clear preserves origin slot");
                break;
            case 7:
                owner.set_bits(cohort, 42, raw); // Regain at the old retained position.
                model[42] = raw;
                break;
            case 8:
                owner.set(cohort, present, 0.0);
                model[present] = 0;
                break;
            case 9:
                owner.set_bits(cohort, missing, 0x7ff8000000000042ULL);
                model[missing] = 0x7ff8000000000042ULL;
                break;
            case 10: {
                const auto before = digest(store);
                require(owner.erase(cohort, missing) == 0, "absent erase returns zero");
                require(digest(store) == before, "absent erase is read-only");
                break;
            }
            default:
                if (!model.empty()) {
                    const auto it = owner.units(cohort).find(present);
                    require(it != owner.units(cohort).end() && it.raw_bits() == model.at(present),
                            "find sees the canonical value");
                }
                break;
            }
            compare(owner, model); // Every operation, including clear, queries and no-ops.
            ++operations;
        }
    }
    require(coverage == 0xfffU, "all twelve mutation/read classes executed");
    std::printf("differential seed=%016llx sizes=9 operations=%zu coverage=%x\n",
                static_cast<unsigned long long>(seed), operations, coverage);
}

void cohort_unit_order_bits() {
    Rig rig;
    ScratchStore store(rig.config(), &rig.backing);
    Owner owner;
    initialize(owner, store);
    Model model;
    const std::array<std::uint64_t, 12> keys{{
        256, 1, UINT64_MAX, 0, 65536, 255, std::uint64_t{1} << 63,
        2, 257, UINT64_MAX - 1, 512, 9}};
    const std::array<std::uint64_t, 12> values{{
        0, 0x8000000000000000ULL, 0x7ff8000000000001ULL, 0xfff8abcdef012345ULL,
        0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0xbff0000000000000ULL,
        1, 0x7ff0000000000042ULL, 0x7fefffffffffffffULL, 0x3ff0000000000000ULL, 42}};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        set_and_compare(owner, model, keys[i], values[i]);
        require(owner.origin_slot(cohort, keys[i]).count == 0, "unit without retained position is kept");
    }
    owner.set(cohort, 65536, 0.0);
    model[65536] = 0;
    compare(owner, model);
    require(owner.units(cohort).contains(65536) && owner.units(cohort).count(65536) == 1,
            "present +zero stays present");
    owner.append_origin(cohort, 777, 0);
    require(!owner.units(cohort).contains(777), "position without unit does not create a unit");
    const auto before = digest(store);
    bool threw = false;
    try { (void)owner.units(cohort).at(777); }
    catch (const std::out_of_range&) { threw = true; }
    require(threw && digest(store) == before, "absent at throws without publication");
}

void cohort_unit_stable_erase() {
    Rig rig;
    ScratchStore store(rig.config(), &rig.backing);
    Owner owner;
    initialize(owner, store);
    Model model;
    for (std::uint64_t i = 0; i < 65; ++i)
        set_and_compare(owner, model, i * 257, 0x7ff8000000000000ULL + i);
    // Repeated root deletion forces two-child successor transplants, then the
    // one-child and singleton cases. No test depends on a particular AVL shape.
    std::size_t erases = 0;
    while (!model.empty()) {
        const auto root = owner.descriptor(cohort).unit_root;
        std::map<std::uint64_t, Owner::Handle> handles;
        auto root_it = owner.units(cohort).end();
        for (auto it = owner.units(cohort).begin(), end = owner.units(cohort).end(); it != end; ++it) {
            handles.emplace(it->first, it.node_handle());
            if (it.node_handle() == root) root_it = it;
        }
        require(root_it != owner.units(cohort).end(), "root is a live enumerated node");
        const auto removed = model.find(root_it->first);
        const auto successor = std::next(removed);
        const auto after = owner.erase(cohort, root_it);
        require((successor == model.end() && after == owner.units(cohort).end())
                    || (successor != model.end() && after->first == successor->first
                        && after.raw_bits() == successor->second
                        && after.node_handle() == handles.at(successor->first)),
                "root erase returns same successor node and payload");
        model.erase(removed);
        compare(owner, model);
        for (auto it = owner.units(cohort).begin(), end = owner.units(cohort).end(); it != end; ++it)
            require(it.node_handle() == handles.at(it->first), "all surviving node identities are stable");
        ++erases;
    }
    require(erases == 65, "every stable-node root erase executed");
}

void cohort_unit_direct_iteration() {
    Rig rig;
    ScratchStore store(rig.config(ScratchOpenMode::create_empty, true), &rig.backing);
    Owner owner;
    initialize(owner, store);
    Model model;
    require(owner.ensure_cohort("other", cohort + 1), "second cohort");
    for (std::uint64_t i = 0; i < 65; ++i) {
        owner.set_bits(cohort, i * 257, i + 1);
        model[i * 257] = i + 1;
        compare(owner, model);
        owner.set_bits(cohort + 1, i * 257, i + 1000); // Interleaved store records.
        compare(owner, model);
    }
    auto stable = owner.units(cohort).find(257);
    const auto id = stable.node_handle();
    set_and_compare(owner, model, 258, 123);
    require(stable.node_handle() == id && stable->first == 257, "insertion preserves iterator identity");
    ++stable;
    require(stable->first == 258, "iterator refreshes changed next link after insertion");
    const auto before = digest(store);
    const auto stats = store.stats();
    const auto allocations = global_allocation::allocations;
    std::size_t count = 0;
    for (auto it = owner.units(cohort).begin(), end = owner.units(cohort).end(); it != end; ++it)
        ++count;
    const auto after = store.stats();
    require(count == model.size(), "reverse internal key order does not affect unit order");
    // 48 per fixed node, plus two exact metadata lookups. No per-step global seek.
    require(after.page_reads - stats.page_reads <= 48 * model.size() + 1024,
            "iteration uses one direct node load per step");
    require(after.page_writes == stats.page_writes && digest(store) == before,
            "iteration does not publish or reclaim");
    require(global_allocation::allocations == allocations, "iteration allocates no heap");
}

void check_slot(const Owner& owner, std::uint64_t who, std::uint64_t incarnation,
                const std::set<std::uint64_t>& positions) {
    const auto slot = owner.origin_slot(who, incarnation);
    require(slot.count == positions.size(), "slot count equals independent retained-position set");
    require(slot.single_position == (positions.size() == 1 ? *positions.begin() : 0),
            "single position is exact only at count one");
}
void cohort_unit_slots() {
    Rig rig;
    ScratchStore store(rig.config(), &rig.backing);
    Owner owner;
    initialize(owner, store);
    require(owner.ensure_cohort("second", cohort + 1), "second slot cohort");
    std::set<std::uint64_t> positions;
    const std::array<std::uint64_t, 3> appends{{9, 15, UINT64_MAX}};
    for (const auto position : appends) {
        owner.fixture_append_origin(cohort, 8, position);
        positions.insert(position);
        check_slot(owner, cohort, 8, positions);
        require(owner.descriptor(cohort).duplicate_slots == (positions.size() > 1 ? 1U : 0U),
                "cohort duplicate total on 1->2 only");
        require(owner.header().duplicate_slots == owner.descriptor(cohort).duplicate_slots,
                "header duplicate total on 1->2 only");
    }
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 2> batch{{{8, 0}, {8, 1}}};
    owner.fixture_append_origins(cohort + 1, batch.begin(), batch.end());
    require(owner.header().duplicate_slots == 2, "header totals span cohorts");
    owner.set_bits(cohort, 8, 0);
    owner.clear(cohort);
    check_slot(owner, cohort, 8, positions);
    owner.retire_origin(cohort, 8, UINT64_MAX); // 3 -> 2.
    positions.erase(UINT64_MAX);
    check_slot(owner, cohort, 8, positions);
    require(owner.header().duplicate_slots == 2, "3->2 keeps duplicate totals");
    owner.retire_origin(cohort, 8, 9, 15); // 2 -> 1.
    positions.erase(9);
    check_slot(owner, cohort, 8, positions);
    require(owner.descriptor(cohort).duplicate_slots == 0 && owner.header().duplicate_slots == 1,
            "2->1 decrements both totals exactly once");
    owner.retire_origin(cohort, 8, 15); // 1 -> 0.
    positions.clear();
    check_slot(owner, cohort, 8, positions);
    owner.retire_origin(cohort + 1, 8, 0, 1);
    owner.retire_origin(cohort + 1, 8, 1);
    require(owner.header().duplicate_slots == 0, "last duplicate retired");
    owner.require_production();
    owner.append_origin(cohort, 8, UINT64_MAX);
    require(owner.origin_slot(cohort, 8).single_position == UINT64_MAX,
            "full-width position and reappend after retirement");
}

void cohort_unit_refusal() {
    Rig rig;
    ScratchStore store(rig.config(), &rig.backing);
    Owner owner;
    initialize(owner, store);
    owner.append_origin(cohort, 7, 0);
    owner.set_bits(cohort, 7, 0x8000000000000000ULL);
    const auto before = digest(store);
    ok(store.begin());
    const auto writes = store.stats().page_writes;
    bool refused = false;
    try { owner.append_origin(cohort, 7, 22); }
    catch (const Failure& error) {
        refused = true;
        const RunFailureInfo& info = error;
        const std::logic_error& standard = error;
        require(error.reason == Reason::duplicate_origin_incarnation
                    && info.run_failure_code() == RunFailureCode::engine_invariant
                    && error.cohort == cohort && error.incarnation == 7
                    && error.old_count == 1 && error.attempted_position == 22
                    && std::strlen(standard.what()) != 0, "typed duplicate refusal diagnostics");
    }
    require(refused, "production second append must throw");
    require(owner.recovery_required() && store.transaction_active(), "explicit recovery required");
    require(store.stats().page_writes == writes, "refusal makes no publication write");
    ok(store.rollback());
    owner.attach(store);
    require(digest(store) == before, "refused append leaves no logical publication");
    require(owner.origin_slot(cohort, 7).count == 1, "refused append did not increment slot");
    owner.fixture_append_origin(cohort, 7, 22);
    require(owner.origin_slot(cohort, 7).count == 2 && owner.header().duplicate_slots == 1,
            "fixture variant preserves multiplicity without refusal");
    owner.attach(store); // A valid fixture store reattaches, even with k>1.
    bool admission_refused = false;
    try { owner.require_production(); }
    catch (const Failure& error) { admission_refused = error.reason == Reason::duplicate_origin_incarnation; }
    require(admission_refused, "constant-time production admission rejects fixture duplicates");
}

void cohort_unit_directory() {
    Rig rig;
    ScratchStore store(rig.config(), &rig.backing);
    Owner owner;
    owner.create(store, 3, UINT64_MAX);
    const std::string common(300, 'x');
    const std::array<std::string, 5> ids{{"", std::string("a\0b", 3),
                                       common + "A", common + "B", "a"}};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto handle = static_cast<std::uint64_t>(i + 1);
        require(owner.ensure_cohort(ids[i], handle), "full id creation");
        require(!owner.ensure_cohort(ids[i], handle), "directory creation is idempotent");
        const auto d = owner.lookup(ids[i]);
        require(d && d->cohort == handle && d->record != 0 && d->unit_count == 0
                    && d->unit_root == 0 && d->unit_first == 0 && d->unit_last == 0
                    && d->answer_bits == 0 && d->unit_epoch == 1 && d->origin_epoch == 1
                    && d->table_epoch == UINT64_MAX, "creation publishes an explicit empty descriptor");
    }
    require(!owner.lookup(std::string("a\0c", 3)) && !owner.lookup(common + "C")
                && !owner.lookup("unknown"), "full-byte equality does not truncate or alias ids");
    owner.publish_position_fields(1, 0xfedcba9876543210ULL, 77, 0x3ff0000000000000ULL);
    owner.set_bits(1, 5, 0x7ff8000000000001ULL);
    const auto opaque = owner.lookup("");
    require(opaque && opaque->position_root == 0xfedcba9876543210ULL
                && opaque->position_count == 77 && opaque->answer_bits == 0x3ff0000000000000ULL,
            "unit edits preserve EI1's opaque fields");
    const auto before = digest(store);
    ok(store.begin());
    ok(store.erase(owner.lookup(ids[2])->record));
    bool failed = false;
    try { (void)owner.lookup(ids[2]); }
    catch (const std::logic_error&) { failed = true; }
    require(failed, "known directory with missing descriptor is invariant failure");
    ok(store.rollback());
    owner.attach(store, 3);
    require(digest(store) == before && owner.lookup(ids[2]).has_value(), "directory rollback restores identity");
}

void cohort_unit_bootstrap() {
    Rig rig;
    ScratchStore store(rig.config(), &rig.backing);
    Owner owner;
    initialize(owner, store);
    const Model model{{0, 0}, {1, 0x7ff8000000000042ULL}, {256, 0x8000000000000000ULL},
                      {std::uint64_t{1} << 63, 1}, {UINT64_MAX, 0xfff0000000000000ULL}};
    owner.bootstrap_from(cohort, model.begin(), model.end());
    compare(owner, model);
    owner.clear(cohort);
    const auto before = digest(store);
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 3> wrong{{{1, 0}, {3, 2}, {2, 9}}};
    ok(store.begin());
    bool rejected = false;
    try { owner.bootstrap_from(cohort, wrong.begin(), wrong.end()); }
    catch (const Failure&) { rejected = true; }
    require(rejected && owner.recovery_required(), "bootstrap rejects nonascending stream");
    ok(store.rollback());
    owner.attach(store);
    require(digest(store) == before && owner.units(cohort).empty(), "partial bootstrap is rolled back");
    owner.bootstrap_from(cohort, model.begin(), model.end());
    compare(owner, model);
}

void cohort_unit_rollback() {
    Rig rig;
    ScratchStore store(rig.config(), &rig.backing);
    Owner owner;
    initialize(owner, store);
    Model model;
    for (std::uint64_t i = 0; i < 33; ++i) set_and_compare(owner, model, i * 3, i + 10);
    owner.append_origin(cohort, 3, 99);
    const auto before = digest(store);
    const auto root = owner.descriptor(cohort).unit_root;
    ok(store.begin());
    owner.clear(cohort);
    owner.set_bits(cohort, UINT64_MAX, 0x7ff8000000000011ULL);
    owner.fixture_append_origin(cohort, 3, 100);
    require(owner.ensure_cohort("provisional", cohort + 1), "provisional directory creation");
    const Model provisional{{UINT64_MAX, 0x7ff8000000000011ULL}};
    compare(owner, provisional);
    require(store.transaction_active(), "all inner operations joined the caller transaction");
    ok(store.rollback());
    owner.attach(store);
    require(digest(store) == before && owner.descriptor(cohort).unit_root == root
                && !owner.lookup("provisional") && owner.header().duplicate_slots == 0
                && owner.origin_slot(cohort, 3).count == 1, "rollback reloads ALL committed owner fields");
    compare(owner, model);
    ok(store.begin());
    owner.set_bits(cohort, 5000, 0);
    const auto aborted = owner.units(cohort).find(5000);
    const auto aborted_handle = aborted.node_handle();
    ok(store.rollback());
    owner.attach(store);
    bool old_iterator_rejected = false;
    try { (void)*aborted; }
    catch (const std::logic_error&) { old_iterator_rejected = true; }
    require(old_iterator_rejected, "reattach discards aborted iterator handles");
    require(digest(store) == before, "handle allocator rolled back with records");
    owner.set_bits(cohort, 5001, 0x3ff0000000000000ULL);
    model[5001] = 0x3ff0000000000000ULL;
    compare(owner, model);
    require(owner.units(cohort).find(5001).node_handle() == aborted_handle,
            "reissued aborted handle is not a stale cached row");
}

void cohort_unit_fault() {
    Rig rig;
    ScratchStore store(rig.config(), &rig.backing);
    Owner owner;
    initialize(owner, store);
    Model model;
    for (std::uint64_t i = 1; i <= 17; ++i) set_and_compare(owner, model, i * 4, i);
    const auto before = digest(store);
    std::size_t faults = 0;
    // Early and later backing writes touch allocation, canonical nodes, repair
    // paths and descriptor publication. The separate commit fault below covers
    // the final public commit boundary as well.
    for (const auto offset : std::array<std::uint64_t, 6>{{1, 2, 4, 8, 16, 32}}) {
        ok(store.begin());
        rig.backing.arm(offset);
        bool failed = false;
        try { owner.set_bits(cohort, 1, 0xfff8abcdef012345ULL); }
        catch (const std::exception&) { failed = true; }
        rig.backing.disarm();
        require(rig.backing.fired && failed && owner.recovery_required(),
                "backing fault actually interrupts canonical publication");
        require(store.transaction_active(), "failed operation leaves caller rollback-only transaction");
        ok(store.rollback());
        bool needs_attach = false;
        try { (void)owner.units(cohort).size(); }
        catch (const Failure& error) { needs_attach = error.reason == Reason::recovery_required; }
        require(needs_attach, "rollback alone cannot reuse poisoned owner frames");
        owner.attach(store);
        require(digest(store) == before, "publication fault rollback preserves exact store digest");
        compare(owner, model);
        ++faults;
    }
    ok(store.begin());
    owner.set_bits(cohort, 1, 123);
    rig.backing.arm(1);
    const auto commit = store.commit(ScratchDurability::buffered);
    rig.backing.disarm();
    require(rig.backing.fired && commit == ScratchStatus::io_error, "commit fault actually fires");
    ok(store.rollback());
    owner.attach(store);
    require(digest(store) == before, "commit fault leaves no half-state after rollback");
    compare(owner, model);
    owner.set_bits(cohort, 1, 0);
    model[1] = 0;
    compare(owner, model);
    std::printf("fault rows executed=%zu plus commit\n", faults);
}

void cohort_unit_reopen() {
    Rig rig;
    std::uint64_t before = 0, epoch = 0, debt = 0, first_job = 0;
    const Model model{{UINT64_MAX, 0x8000000000000000ULL}};
    {
        ScratchStore store(rig.config(), &rig.backing);
        Owner owner;
        initialize(owner, store);
        for (std::uint64_t i = 0; i < 65; ++i) owner.set_bits(cohort, i, i + 1);
        owner.clear(cohort);
        owner.set_bits(cohort, UINT64_MAX, model.begin()->second);
        compare(owner, model);
        before = digest(store);
        epoch = owner.descriptor(cohort).unit_epoch;
        debt = owner.header().reclaim_nodes;
        first_job = owner.header().reclaim_first;
        require(debt > Owner::reclamation_budget && first_job != 0, "reopen fixture has persisted GC debt");
    }
    {
        ScratchStore store(rig.config(ScratchOpenMode::reopen), &rig.backing);
        ok(store.status());
        const auto writes = store.stats().page_writes;
        Owner owner;
        owner.attach(store);
        compare(owner, model);
        require(owner.lookup("cohort")->unit_epoch == epoch
                    && owner.header().reclaim_nodes == debt
                    && owner.header().reclaim_first == first_job, "reopen resumes same generation/cursor");
        require(digest(store) == before && store.stats().page_writes == writes,
                "reopen and queries are read-only, including GC");
        owner.set_bits(cohort, UINT64_MAX, model.begin()->second);
        require(owner.header().reclaim_nodes == debt - Owner::reclamation_budget,
                "first later mutation services reopened cursor");
        compare(owner, model);
    }
}

void cohort_unit_reclaim() {
    Rig rig;
    ScratchStore store(rig.config(), &rig.backing);
    Owner owner;
    initialize(owner, store);
    constexpr std::uint64_t live = 17;
    // At most current + detached populations, two job/header records and the
    // fixed directory/descriptor/slot metadata. No allowance grows with cycles.
    constexpr std::uint64_t record_ceiling = 2 * live + 8;
    // 128 pages per permitted record covers cells, both indexes, allocation and
    // journal images at the geometry floor; 256 covers fixed store metadata.
    constexpr std::uint64_t extent_ceiling = 128 * record_ceiling + 256;
    const auto resident = store.stats().resident_bytes;
    std::uint64_t peak_records = 0, peak_pages = 0;
    Model model;
    owner.append_origin(cohort, 4, 77);
    for (std::uint64_t cycle = 0; cycle < 64; ++cycle) {
        for (std::uint64_t i = 0; i < live; ++i) {
            set_and_compare(owner, model, i * 4, cycle + i);
            peak_records = std::max(peak_records, store.record_count());
            peak_pages = std::max(peak_pages, store.stats().backing_pages);
            require(store.record_count() <= record_ceiling
                        && store.stats().backing_pages <= extent_ceiling
                        && store.stats().resident_bytes == resident, "fixed-live churn stays bounded");
        }
        require(owner.header().reclaim_jobs == 0 && owner.header().reclaim_nodes == 0,
                "all old jobs retire during regain");
        owner.publish_position_fields(cohort, 0, 0, 0x3ff0000000000000ULL);
        owner.clear(cohort);
        model.clear();
        compare(owner, model);
        const auto cleared = owner.descriptor(cohort);
        require(cleared.answer_bits == 0 && cleared.unit_count == 0
                    && cleared.unit_root == 0 && cleared.unit_first == 0 && cleared.unit_last == 0,
                "generation clear detaches units and zeroes answer");
        require(owner.origin_slot(cohort, 4).count == 1, "clear does not reclaim origin slots");
        const auto jobs = owner.header().reclaim_jobs;
        const auto old_epoch = cleared.unit_epoch;
        owner.clear(cohort); // Services a fixed budget, never enqueues empty generations.
        compare(owner, model);
        require(owner.descriptor(cohort).unit_epoch == old_epoch
                    && owner.header().reclaim_jobs <= jobs, "repeated empty clear enqueues nothing");
    }
    for (unsigned service = 0; service < live + 1; ++service) owner.clear(cohort);
    require(owner.header().reclaim_jobs == 0 && owner.header().reclaim_nodes == 0
                && store.record_count() == 4, "nodes AND job records physically retired");
    std::printf("churn cycles=64 U=17 peak_records=%llu/%llu peak_pages=%llu/%llu resident=%llu\n",
        static_cast<unsigned long long>(peak_records), static_cast<unsigned long long>(record_ceiling),
        static_cast<unsigned long long>(peak_pages), static_cast<unsigned long long>(extent_ceiling),
        static_cast<unsigned long long>(resident));
}

struct Cost { std::uint64_t loads = 0, allocations = 0, peak_bytes = 0; };
std::size_t window_peak = 0;
void allocation_seen(std::size_t) {
    window_peak = std::max(window_peak, global_allocation::live_bytes);
}
template<class Work>
Cost measure(ScratchStore& store, Work&& work) {
    const auto stats = store.stats();
    const auto allocations = global_allocation::allocations;
    const auto live = global_allocation::live_bytes;
    window_peak = live;
    global_allocation::on_recorded = &allocation_seen;
    try { work(); }
    catch (...) { global_allocation::on_recorded = nullptr; throw; }
    global_allocation::on_recorded = nullptr;
    const auto after = store.stats();
    require(!store.transaction_active(), "cost includes commit, not only provisional edits");
    require(stats.resident_bytes == after.resident_bytes && global_allocation::live_bytes == live,
            "fixed resident store and no retained allocation");
    return {after.page_reads - stats.page_reads, global_allocation::allocations - allocations,
            window_peak - live};
}
std::uint64_t log_ceiling(std::uint64_t n) {
    std::uint64_t m = 0, power = 1;
    while (power < std::max<std::uint64_t>(n, 2)) { power <<= 1; ++m; }
    return m;
}
void cohort_unit_cost() {
    constexpr std::uint64_t c = std::uint64_t{1} << 20;
    constexpr std::uint64_t growth_slack = 4096, allocation_allowance = 0;
    constexpr std::array<std::uint64_t, 4> sizes{{17, 65, 257, 1025}};
    constexpr std::array<const char*, 7> names{{"insert-absent", "set-existing", "erase-key",
        "erase-iterator", "clear", "regain", "find"}};
    std::array<std::array<Cost, names.size()>, sizes.size()> costs{};
    for (std::size_t sample = 0; sample < sizes.size(); ++sample) {
        Rig rig;
        ScratchStore store(rig.config(), &rig.backing);
        Owner owner;
        initialize(owner, store);
        const auto u = sizes[sample];
        Model model;
        for (std::uint64_t i = 1; i <= u; ++i) {
            owner.set_bits(cohort, i * 4, i);
            model[i * 4] = i;
        }
        compare(owner, model);
        auto& cost = costs[sample];
        cost[0] = measure(store, [&] { require(owner.insert_bits(cohort, 1, 0), "measured insertion runs"); });
        model[1] = 0; compare(owner, model);
        owner.erase(cohort, 1); model.erase(1); compare(owner, model);
        const auto middle = (u / 2 + 1) * 4;
        cost[1] = measure(store, [&] { owner.set_bits(cohort, middle, 42); });
        model[middle] = 42; compare(owner, model);
        cost[2] = measure(store, [&] { require(owner.erase(cohort, 4) == 1, "measured key erase runs"); });
        model.erase(4); compare(owner, model);
        owner.set_bits(cohort, 4, 1); model[4] = 1; compare(owner, model);
        cost[3] = measure(store, [&] {
            const auto found = owner.units(cohort).find(middle);
            require(found != owner.units(cohort).end(), "measured iterator is present");
            (void)owner.erase(cohort, found);
        });
        model.erase(middle); compare(owner, model);
        owner.set_bits(cohort, middle, 42); model[middle] = 42; compare(owner, model);
        cost[4] = measure(store, [&] { owner.clear(cohort); });
        model.clear(); compare(owner, model);
        cost[5] = measure(store, [&] { owner.set_bits(cohort, 4, 0); });
        model[4] = 0; compare(owner, model);
        cost[6] = measure(store, [&] {
            const auto found = owner.units(cohort).find(4);
            require(found != owner.units(cohort).end() && found.raw_bits() == 0, "measured find runs");
        });
        compare(owner, model);
        for (std::size_t op = 0; op < names.size(); ++op) {
            const auto m = log_ceiling(u);
            const auto ratio = sample && costs[sample - 1][op].loads
                ? static_cast<double>(cost[op].loads) / costs[sample - 1][op].loads : 0.0;
            std::printf("cost U=%llu op=%s loads=%llu ratio=%.6f allocations=%llu peak_bytes=%llu c=%llu\n",
                static_cast<unsigned long long>(u), names[op],
                static_cast<unsigned long long>(cost[op].loads), ratio,
                static_cast<unsigned long long>(cost[op].allocations),
                static_cast<unsigned long long>(cost[op].peak_bytes),
                static_cast<unsigned long long>(c));
            std::fflush(stdout);
            require(cost[op].loads > 0 && cost[op].loads <= c * m * m, "derived log-squared absolute bound");
            require(cost[op].allocations == 0 && cost[op].peak_bytes == 0, "operation has fixed allocation-free workspace");
            if (sample) {
                const auto old_m = log_ceiling(sizes[sample - 1]);
                require(cost[op].loads * old_m * old_m
                            <= costs[sample - 1][op].loads * m * m + growth_slack * old_m * old_m,
                        "growth within log-squared ratio plus fixed slack");
            }
            if (sample + 1 == sizes.size())
                require(cost[op].allocations <= costs[0][op].allocations + allocation_allowance,
                        "U=1025 allocations <= U=17 plus fixed allowance");
        }
    }
}

struct Test { const char* name; void (*body)(); };
const Test cases[] = {
    {"cohort_unit_differential", &cohort_unit_differential},
    {"cohort_unit_order_bits", &cohort_unit_order_bits},
    {"cohort_unit_stable_erase", &cohort_unit_stable_erase},
    {"cohort_unit_direct_iteration", &cohort_unit_direct_iteration},
    {"cohort_unit_slots", &cohort_unit_slots},
    {"cohort_unit_refusal", &cohort_unit_refusal},
    {"cohort_unit_directory", &cohort_unit_directory},
    {"cohort_unit_bootstrap", &cohort_unit_bootstrap},
    {"cohort_unit_rollback", &cohort_unit_rollback},
    {"cohort_unit_fault", &cohort_unit_fault},
    {"cohort_unit_reopen", &cohort_unit_reopen},
    {"cohort_unit_reclaim", &cohort_unit_reclaim},
    {"cohort_unit_cost", &cohort_unit_cost},
};
} // namespace

int main(int argc, char** argv) {
    global_allocation::recording = true;
    bool selected = false;
    for (const auto& test : cases) {
        if (argc > 1 && std::strcmp(test.name, argv[1])) continue;
        selected = true;
        try {
            test.body();
            std::printf("%s: PASS\n", test.name);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s: FAIL: %s\n", test.name, error.what());
            return 1; // A failed control stops the sequence.
        }
    }
    if (!selected) std::fprintf(stderr, "unknown cohort unit owner case\n");
    return selected ? 0 : 1;
}
