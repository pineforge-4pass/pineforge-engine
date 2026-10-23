// EI1 r2: real borrowed ScratchStore, E3 geometry (512 bytes, TWO cache slots).
// All execution, including fail-before compilation, belongs to the box.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"
#include "cohort_exposure_oracle.hpp"
#include <pineforge/source/cohort_exposure_index.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pineforge::detail;
using Index = pineforge::source::CohortExposureIndex;
using Owner = pineforge::source::CohortUnitOwner;
using Failure = pineforge::source::ExposureOwnerFailure;
using Reason = pineforge::source::ExposureOwnerError;
namespace ce = cohort_exposure_test;
using Model = std::map<std::uint64_t, std::uint64_t>;
constexpr std::size_t page_bytes = 512;
constexpr std::uint64_t cohort = 19;
constexpr std::uint64_t seed = UINT64_C(0x5345344549315232);
static_assert(sizeof(std::size_t) >= sizeof(std::uint64_t), "E3 must represent max-u64 positions");
static_assert(sizeof(Index) <= 4096, "three 936-byte frames, 128 handles, fixed metadata");
std::size_t checkpoints = 0;

void require(bool value, const char* message) {
    ++checkpoints;
    if (!value) throw std::runtime_error(message);
}
void ok(ScratchStatus status) { require(status == ScratchStatus::ok, "scratch operation succeeds"); }
void word(std::uint64_t actual, std::uint64_t expected, const char* message) {
    if (actual != expected)
        std::fprintf(stderr, "actual=%016llx expected=%016llx\n",
                     static_cast<unsigned long long>(actual), static_cast<unsigned long long>(expected));
    require(actual == expected, message);
}
struct Generator {
    std::uint64_t state = seed;
    std::uint64_t next() {
        auto z = (state += UINT64_C(0x9e3779b97f4a7c15));
        z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
        return z ^ (z >> 31);
    }
};
struct OrderCounter { std::uint64_t comparisons = 0; };
ScratchStatus lexical(void* context, const ScratchByteSource& a, const ScratchByteSource& b,
                      int& order) noexcept {
    ++static_cast<OrderCounter*>(context)->comparisons;
    std::array<std::byte, 32> left{}, right{};
    const auto common = std::min(a.size(), b.size());
    for (std::uint64_t at = 0; at < common;) {
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(left.size(), common - at));
        auto status = a.read(at, {left.data(), n});
        if (status != ScratchStatus::ok) return status;
        status = b.read(at, {right.data(), n});
        if (status != ScratchStatus::ok) return status;
        const auto comparison = std::memcmp(left.data(), right.data(), n);
        if (comparison) { order = comparison < 0 ? -1 : 1; return ScratchStatus::ok; }
        at += n;
    }
    order = a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
    return ScratchStatus::ok;
}

// This fixture follows NO files. It uses the existing FaultBacking idiom:
// failures are injected into actual store backing/journal/publication writes.
class FaultBacking final : public ScratchBacking {
public:
    explicit FaultBacking(ScratchBuffer bytes)
        : ScratchBacking(ScratchBackingKind::test_memory, page_bytes), memory_(bytes, page_bytes) {}
    std::uint64_t page_count() const noexcept override { return memory_.page_count(); }
    std::uint64_t writes = 0, fail_write = 0;
    bool fired = false;
    void arm(std::uint64_t nth) { fail_write = writes + nth; fired = false; }
    void disarm() { fail_write = 0; }
private:
    ScratchStatus do_read_page(ScratchPageId id, ScratchBuffer out) const noexcept override {
        return memory_.read_page(id, out);
    }
    ScratchStatus do_write_page(ScratchPageId id, ScratchBytes in) noexcept override {
        ++writes;
        if (fail_write && writes == fail_write) { fired = true; return ScratchStatus::io_error; }
        return memory_.write_page(id, in);
    }
    ScratchStatus do_sync(ScratchDurability d) noexcept override { return memory_.sync(d); }
    FixedMemoryScratchBacking memory_;
};
struct Rig {
    // Explicit test memory, provisioned before measurements; not resident
    // engine workspace and not an implicit backing selection.
    std::vector<std::byte> bytes;
    FaultBacking backing;
    OrderCounter order{};
    Rig() : bytes(32768 * page_bytes), backing({bytes.data(), bytes.size()}) {}
    ScratchStoreConfig config(ScratchOpenMode mode = ScratchOpenMode::create_empty) {
        return {2, page_bytes, 1, 1, {UINT64_C(0x4549315232), &order, &lexical}, mode};
    }
};
std::uint64_t digest(const ScratchStore& store) {
    std::array<std::byte, page_bytes> page{};
    std::uint64_t result = 0;
    ok(store.state_digest({page.data(), page.size()}, result));
    return result;
}
std::uint64_t oracle(const Model& model) {
    std::vector<ce::Row> rows;
    for (const auto& row : model)
        rows.push_back({static_cast<std::size_t>(row.first), row.first, ce::from_bits(row.second)});
    return ce::bits(ce::oracle(rows)); // Independent sequential floating-point fold.
}
enum class Action { insert, replace, erase };
struct Subject {
    ScratchStore& store;
    Owner owner;
    Index index;
    explicit Subject(ScratchStore& store, bool create = true) : store(store) {
        ok(store.status());
        require(store.page_size() == page_bytes, "geometry remains 512-byte pages");
        if (create) {
            owner.create(store);
            require(owner.ensure_cohort("cohort", cohort), "real descriptor created");
        } else owner.attach(store);
        index.attach(store);
    }
    Index::Root current() const {
        const auto d = owner.descriptor(cohort); // Reload, never a cached root.
        return {d.position_root, d.position_count, d.table_epoch, d.cohort, d.unit_epoch};
    }
    void publish(const Index::Edit& edit) {
        require(store.transaction_active(), "descriptor joins the edit's outer transaction");
        owner.publish_position_fields(cohort, edit.root.root, edit.root.count, edit.answer_bits);
        require(store.transaction_active(), "neither index nor publisher committed the caller transaction");
    }
    Index::Edit pending(Action action, std::uint64_t position, std::uint64_t raw = 0) {
        const auto root = current();
        if (action == Action::insert) return index.insert(root, position, raw);
        if (action == Action::replace) return index.replace(root, position, raw);
        return index.erase(root, position);
    }
    Index::Edit edit(Action action, std::uint64_t position, std::uint64_t raw = 0) {
        ok(store.begin());
        const auto changed = pending(action, position, raw);
        publish(changed);
        ok(store.commit(ScratchDurability::buffered));
        return changed;
    }
    void compare(const Model& model) {
        const auto root = current();
        const auto expected = oracle(model);
        word(root.count, model.size(), "descriptor leaf count equals model");
        word(index.answer(root), expected, "stored transition answer equals independent fold");
        word(owner.descriptor(cohort).answer_bits, expected, "Edit answer was republished atomically");
        index.validate(root);
        require(!index.recovery_required(), "real validation completed");
    }
    void recover() {
        owner.attach(store);
        index.attach(store);
    }
    void clear() {
        ok(store.begin());
        auto root = current();
        auto detached = index.detach(root);
        publish({root, 0, 0, 0});
        // Fixture cleanup is explicit V-step work, not a flat production
        // clear claim. Round 3 persists detached in EI2's reclaim job instead.
        while (detached.root) (void)index.reclaim_step(detached);
        ok(store.commit(ScratchDurability::buffered));
    }
};

std::uint64_t choose(const Model& model, std::uint64_t random) {
    auto at = model.begin();
    std::advance(at, static_cast<std::ptrdiff_t>(random % model.size()));
    return at->first;
}
std::uint64_t absent(const Model& model, std::uint64_t position) {
    while (model.count(position)) ++position; // Test-only model work, unsigned wrap defined.
    return position;
}
std::vector<std::uint64_t> payloads() {
    std::vector<std::uint64_t> result;
    for (const auto& row : ce::adversarial_corpus())
        for (const auto& m : row.mutations)
            if (m.edit == ce::Edit::insert || m.edit == ce::Edit::set)
                result.push_back(ce::bits(m.value));
    return result;
}
void cohort_index_differential() {
    const auto values = payloads();
    require(!values.empty(), "E3 adversarial operands available");
    Generator rng;
    for (const auto initial : {0U, 1U, 2U, 3U, 7U, 17U, 33U, 65U, 257U}) {
        Rig rig;
        ScratchStore store(rig.config(), &rig.backing);
        Subject subject(store);
        Model model;
        subject.compare(model);
        for (std::uint64_t i = 0; i < initial; ++i) {
            const auto random = rng.next();
            std::uint64_t position = i;
            switch (i % 5) {
            case 0: position = i; break;
            case 1: position = i << 48; break;
            case 2: position = UINT64_MAX - i; break;
            case 3: position = UINT64_C(1) << static_cast<unsigned>(i % 64); break;
            default: position = random; break;
            }
            position = absent(model, position);
            const auto value_index = rng.next();
            const auto raw = values[value_index % values.size()];
            const auto edit = subject.edit(Action::insert, position, raw);
            model[position] = raw;
            word(edit.root.count, model.size(), "insert returns new count");
            subject.compare(model);
        }
        const unsigned edits = initial <= 17 ? 48 : 12;
        for (unsigned step = 0; step < edits; ++step) {
            const auto random_position = rng.next();
            const auto random_value = rng.next(); // RNG calls sequenced before arguments.
            const auto raw = values[random_value % values.size()];
            if (model.empty() || step % 3 == 0) {
                auto position = random_position;
                if (step % 4 == 0) position = step;
                if (step % 4 == 1) position = UINT64_MAX - step;
                if (step % 4 == 2) position = (UINT64_C(1) << (step % 64)) ^ 1;
                position = absent(model, position);
                subject.edit(Action::insert, position, raw);
                model[position] = raw;
            } else {
                const auto position = choose(model, random_position);
                if (step % 3 == 1) {
                    subject.edit(Action::replace, position, raw);
                    model[position] = raw;
                } else {
                    subject.edit(Action::erase, position);
                    model.erase(position);
                }
            }
            subject.compare(model);
        }
        std::printf("differential initial=%u mutations=%u live=%zu\n", initial, edits, model.size());
    }
    // Analytic, order-sensitive witness and deletion/replacement of the max
    // exponent; catches reversed composition and stale ancestor summaries.
    Rig rig;
    ScratchStore store(rig.config(), &rig.backing);
    Subject subject(store);
    Model model;
    for (const auto& row : Model{{0, ce::bits(1.0)}, {1, ce::bits(1.0)}, {2, ce::bits(0x1p53)}}) {
        subject.edit(Action::insert, row.first, row.second);
        model[row.first] = row.second;
        subject.compare(model);
    }
    word(oracle(model), UINT64_C(0x4340000000000001), "two small operands precede 2^53");
    subject.edit(Action::replace, 2, ce::bits(0.25)); model[2] = ce::bits(0.25);
    subject.compare(model);
    subject.edit(Action::erase, 1); model.erase(1);
    subject.compare(model);
    // Every independent E1 hand vector, with a check after every prefix.
    for (const auto& hand : ce::e1_hand_vectors()) {
        subject.clear(); model.clear();
        for (std::size_t i = 0; i < hand.operands.size(); ++i) {
            const auto raw = ce::bits(hand.operands[i]);
            subject.edit(Action::insert, i, raw); model[i] = raw;
            subject.compare(model);
        }
        word(oracle(model), hand.expected_bits, "hand arithmetic pins the independent fold");
    }
    subject.clear(); model.clear();
    for (const auto& row : Model{{0, ce::bits(0x1p100)}, {1, ce::bits(0.125)},
                                 {2, ce::bits(0.25)}, {3, ce::bits(0.5)}}) {
        subject.edit(Action::insert, row.first, row.second);
        model[row.first] = row.second;
        subject.compare(model);
    }
    subject.edit(Action::erase, 0); model.erase(0);
    subject.compare(model); // Grandparent's maximum exponent must fall.
    subject.edit(Action::replace, 3, ce::bits(0x1p200)); model[3] = ce::bits(0x1p200);
    subject.compare(model);
    // A full 64-ancestor Patricia comb, including the unsigned top bit.
    subject.clear(); model.clear();
    subject.edit(Action::insert, 0, ce::bits(1.0)); model[0] = ce::bits(1.0);
    subject.compare(model);
    for (unsigned bit = 0; bit < 64; ++bit) {
        const auto position = UINT64_C(1) << bit;
        subject.edit(Action::insert, position, ce::bits(1.0)); model[position] = ce::bits(1.0);
        subject.compare(model);
    }
    const auto deepest = subject.edit(Action::replace, 0, ce::bits(0.5));
    const auto depth_metrics = subject.index.metrics();
    require(depth_metrics.path_length == 64 && deepest.puts == 65,
            "full word-depth path recomposes exactly 64 ancestors plus its leaf");
    model[0] = ce::bits(0.5); subject.compare(model);
}

// Only the fixture's multiset selection lives in RAM. Arithmetic always uses
// the persistent tree, and each distinct position has exactly one leaf.
class Candidate final : public ce::ExposureCandidate {
public:
    Candidate() : store_(rig_.config(), &rig_.backing), subject_(store_) {}
    std::size_t queries = 0, inserts = 0, sets = 0, erases = 0, clears = 0;
    void insert(std::size_t position, std::uint64_t incarnation, double value) override {
        ++inserts;
        const auto raw = ce::bits(value);
        for (auto& row : rows_) if (row.incarnation == incarnation) row.raw = raw;
        rows_.push_back({position, incarnation, raw});
        synchronize();
    }
    void set(std::uint64_t incarnation, double value) override {
        ++sets;
        const auto raw = ce::bits(value);
        for (auto& row : rows_) if (row.incarnation == incarnation) row.raw = raw;
        synchronize();
    }
    void erase(std::uint64_t incarnation) override {
        ++erases;
        rows_.erase(std::remove_if(rows_.begin(), rows_.end(),
            [incarnation](const Row& row) { return row.incarnation == incarnation; }), rows_.end());
        synchronize();
    }
    void clear() override {
        ++clears;
        subject_.clear();
        rows_.clear(); selected_.clear();
    }
    double exposure() override {
        ++queries;
        const auto root = subject_.current();
        subject_.index.validate(root);
        const auto bits = subject_.owner.descriptor(cohort).answer_bits;
        word(subject_.index.answer(root), bits, "published candidate answer agrees with stored tree");
        return ce::from_bits(bits);
    }
private:
    struct Row { std::size_t position; std::uint64_t incarnation, raw; };
    void synchronize() {
        auto ordered = rows_;
        std::stable_sort(ordered.begin(), ordered.end(), [](const Row& a, const Row& b) {
            return a.position < b.position
                || (a.position == b.position && a.incarnation < b.incarnation);
        });
        Model desired;
        for (const auto& row : ordered) desired.emplace(row.position, row.raw);
        ok(store_.begin());
        for (const auto& row : selected_) {
            const auto next = desired.find(row.first);
            if (next == desired.end()) subject_.publish(subject_.pending(Action::erase, row.first));
            else if (next->second != row.second)
                subject_.publish(subject_.pending(Action::replace, row.first, next->second));
        }
        for (const auto& row : desired)
            if (!selected_.count(row.first))
                subject_.publish(subject_.pending(Action::insert, row.first, row.second));
        ok(store_.commit(ScratchDurability::buffered));
        selected_ = std::move(desired);
    }
    Rig rig_;
    ScratchStore store_;
    Subject subject_;
    std::vector<Row> rows_;
    Model selected_;
};
void cohort_index_exposure_candidate() {
    const auto prepared = ce::prepare(ce::adversarial_corpus());
    std::size_t expected = 0;
    for (const auto& row : prepared) {
        require(!row.checkpoints.empty(), "every E3 row has real checkpoints");
        expected += 1 + row.checkpoints.size();
    }
    Candidate candidate;
    const auto verdict = ce::exercise_candidate(candidate, prepared);
    for (const auto& mismatch : verdict.rejected_cases)
        std::fprintf(stderr, "E3=%s step=%zu expected=%016llx actual=%016llx\n",
            mismatch.case_name.c_str(), mismatch.step,
            static_cast<unsigned long long>(mismatch.expected),
            static_cast<unsigned long long>(mismatch.actual));
    require(expected != 0 && verdict.comparisons == expected && candidate.queries == expected,
            "all E3 checkpoints including empty queries actually executed");
    require(candidate.inserts && candidate.sets && candidate.erases && candidate.clears,
            "all four fixture mutation families executed");
    require(verdict.mismatches == 0 && verdict.rejected_cases.empty(), "whole E3 corpus accepted");
    std::printf("E3 cases=%zu checkpoints=%zu mismatches=%zu\n",
                prepared.size(), verdict.comparisons, verdict.mismatches);
}

// COST DERIVATION, before measurement:
// E1 P3/P6: h<=64, replacement h+1 node puts, V_eval<=1+4092h.
// E2 Q2 and the 512-byte store geometry: d<=12, 936-byte values occupy TWO
// 472-byte blob pages; 64-byte keys occupy two 56-byte cells. Including the
// key identity read, point/read fits <=64 page references per logical load.
// Allow 4096*(H+1) references for one keyed put/erase or ordered lookup:
// <=6*64 comparisons + 58*16 split/shift + 12*64 handle repair + 4*64 blob
// work + 3*512 journal/free-list/commit <4096. E2 Q2's rounded allowance
// includes commit. Here R<=2U+8 and dense positions give h<=m+1 and H<=m+3,
// m=ceil(log2 U), U>=17. <=64 ID probes occur on insert only. Thus
//   reads <=64*(4*(m+1)+4+1+4092*(m+1))
//           +4096*(m+4)*(m+1+8+64)+4096 < 2^22*m^2.
// The ratio gate adds 4096 references for one fixed metadata/journal unit,
// independently of observations. A second, baseline-relative multiplicative
// gate allows 4: at most a factor 2 each for the fresh ordered-index and
// handle-radix depth ceilings when crossing a geometry threshold (58/29 and
// 15/60), beyond the m^2 normalization. Comparing against U=17, not only the
// preceding sample, prevents repeated slack from hiding a linear rewrite.
// Zero allocation allowance follows directly
// from the three resident frames + fixed path and the store's no-growth API.
struct Cost {
    std::uint64_t loads = 0, puts = 0, allocations = 0, peak_bytes = 0;
    std::uint64_t resident = 0, path = 0, logical_loads = 0, key_searches = 0;
};
std::size_t window_peak = 0;
void allocation_seen(std::size_t) {
    window_peak = std::max(window_peak, global_allocation::live_bytes);
}
template<class Work>
Cost measure(Subject& s, Work&& work) {
    const auto before = s.store.stats();
    const auto allocations = global_allocation::allocations;
    const auto live = global_allocation::live_bytes;
    window_peak = live;
    global_allocation::on_recorded = &allocation_seen;
    Index::Edit edit;
    try { edit = work(); }
    catch (...) { global_allocation::on_recorded = nullptr; throw; }
    global_allocation::on_recorded = nullptr;
    const auto after = s.store.stats();
    const auto metrics = s.index.metrics();
    require(!s.store.transaction_active(), "measured edit includes descriptor and commit");
    require(before.resident_bytes == after.resident_bytes && global_allocation::live_bytes == live,
            "no retained allocation, fixed borrowed store workspace");
    return {after.page_reads - before.page_reads, edit.puts,
        global_allocation::allocations - allocations, window_peak - live,
        after.resident_bytes + s.index.resident_bytes() + sizeof(Owner),
        metrics.path_length, edit.loads, metrics.key_searches};
}
std::uint64_t log_ceiling(std::uint64_t u) {
    std::uint64_t m = 0, power = 1;
    while (power < u) { power <<= 1; ++m; }
    return m;
}
void cohort_index_cost() {
    constexpr std::array<std::uint64_t, 4> sizes{{17, 65, 257, 1025}};
    constexpr std::array<const char*, 3> names{{"insert", "replace", "erase"}};
    constexpr std::uint64_t c = UINT64_C(1) << 22;
    constexpr std::uint64_t growth_slack = 4096, allocation_allowance = 0;
    constexpr std::uint64_t ratio_slack = 4;
    std::array<std::array<Cost, 3>, 4> costs{};
    for (std::size_t sample = 0; sample < sizes.size(); ++sample) {
        Rig rig;
        ScratchStore store(rig.config(), &rig.backing);
        Subject subject(store);
        Model model;
        const auto u = sizes[sample];
        for (std::uint64_t i = 0; i < u; ++i) {
            subject.edit(Action::insert, i * 4, ce::bits(1.0));
            model[i * 4] = ce::bits(1.0);
        }
        subject.compare(model);
        auto& sample_cost = costs[sample];
        sample_cost[0] = measure(subject, [&] {
            return subject.edit(Action::insert, 1, ce::bits(0.25));
        });
        model[1] = ce::bits(0.25); subject.compare(model);
        subject.edit(Action::erase, 1); model.erase(1);
        const auto middle = (u / 2) * 4;
        sample_cost[1] = measure(subject, [&] {
            return subject.edit(Action::replace, middle, ce::bits(0.75));
        });
        model[middle] = ce::bits(0.75); subject.compare(model);
        sample_cost[2] = measure(subject, [&] { return subject.edit(Action::erase, middle); });
        model.erase(middle); subject.compare(model);
        const auto root = subject.current(); // Excluded descriptor key lookup.
        const auto comparisons = rig.order.comparisons;
        const auto answer = subject.index.answer(root);
        const auto answer_metrics = subject.index.metrics();
        word(answer, oracle(model), "cost sample still computes the real answer");
        require(rig.order.comparisons == comparisons && answer_metrics.key_searches == 0,
                "evaluation makes ZERO ordered comparisons: all children read by handle");
        for (std::size_t op = 0; op < names.size(); ++op) {
            const auto& value = sample_cost[op];
            const auto m = log_ceiling(u);
            const auto ratio = sample && costs[sample - 1][op].loads
                ? static_cast<double>(value.loads) / costs[sample - 1][op].loads : 0.0;
            const auto put_ratio = sample && costs[sample - 1][op].puts
                ? static_cast<double>(value.puts) / costs[sample - 1][op].puts : 0.0;
            std::printf("cost U=%llu op=%s loads=%llu ratio=%.6f puts=%llu put_ratio=%.6f "
                        "h=%llu node_loads=%llu key_searches=%llu allocations=%llu peak_bytes=%llu "
                        "resident=%llu c=%llu ratio_slack=%llu\n",
                static_cast<unsigned long long>(u), names[op],
                static_cast<unsigned long long>(value.loads), ratio,
                static_cast<unsigned long long>(value.puts), put_ratio,
                static_cast<unsigned long long>(value.path),
                static_cast<unsigned long long>(value.logical_loads),
                static_cast<unsigned long long>(value.key_searches),
                static_cast<unsigned long long>(value.allocations),
                static_cast<unsigned long long>(value.peak_bytes),
                static_cast<unsigned long long>(value.resident), static_cast<unsigned long long>(c),
                static_cast<unsigned long long>(ratio_slack));
            std::fflush(stdout);
            require(value.loads > 0 && value.loads <= c * m * m, "derived absolute log-squared bound");
            require(value.path <= 64, "actual radix path has at most 64 ancestors");
            require(value.allocations == 0 && value.peak_bytes == 0, "operation is allocation-free");
            require(op == 0 ? value.key_searches <= 64 : value.key_searches == 0,
                    "only insertion's bounded ID allocator may perform explicit ordered searches");
            if (op == 1)
                require(value.puts == value.path + 1 && value.puts <= value.path + 2,
                        "replace writes leaf and every ancestor once; caller descriptor is the extra put");
            if (sample) {
                const auto old_m = log_ceiling(sizes[sample - 1]);
                require(value.loads * old_m * old_m <= costs[sample - 1][op].loads * m * m
                            + growth_slack * old_m * old_m, "log-squared ratio plus fixed metadata slack");
                const auto base_m = log_ceiling(sizes[0]);
                require(value.loads * base_m * base_m
                            <= costs[0][op].loads * m * m * ratio_slack,
                        "baseline growth within squared log ratio times derived multiplicative slack");
                require(value.puts * old_m * old_m <= costs[sample - 1][op].puts * m * m
                            + 2 * old_m * old_m, "put growth with two splice-record allowance");
                require(value.resident == costs[0][op].resident, "resident bytes flat across U");
            }
            if (sample + 1 == sizes.size())
                require(value.allocations <= costs[0][op].allocations + allocation_allowance,
                        "1025 allocation count no greater than 17 plus zero allowance");
        }
    }
    // Strictly increasing exponents force crossings throughout the binary64
    // universe. Position 0 is minimum subnormal; each successive normal power
    // meets or exceeds the accumulated previous powers, up to exponent 2046.
    Rig rig;
    ScratchStore store(rig.config(), &rig.backing);
    Subject subject(store);
    Model crossing;
    subject.edit(Action::insert, 0, 1); crossing[0] = 1;
    for (std::uint64_t exponent = 1; exponent <= 2046; ++exponent) {
        const auto raw = exponent << 52;
        subject.edit(Action::insert, exponent, raw);
        crossing[exponent] = raw;
    }
    const auto root = subject.current();
    std::array<std::byte, 8> height_bytes{};
    ok(store.read(root.root, ScratchField::value, 56, {height_bytes.data(), height_bytes.size()}));
    const auto h = pineforge::detail::working_store::word(height_bytes.data());
    const auto actual = subject.index.answer(root);
    const auto measured = subject.index.metrics();
    word(actual, oracle(crossing), "many-crossing evaluator equals sequential fold");
    require(h <= 64 && measured.node_visits <= 1 + 4092 * h, "E1 P3 node-visit bound");
    require(measured.real_additions > 1000 && measured.real_additions <= 2046,
            "many real crossings executed, at most 2046 additions");
    subject.index.validate(root);
    std::printf("crossings V=%llu h=%llu visits=%llu additions=%llu\n",
        static_cast<unsigned long long>(root.count), static_cast<unsigned long long>(h),
        static_cast<unsigned long long>(measured.node_visits),
        static_cast<unsigned long long>(measured.real_additions));
}

void fault_sweep(Action action) {
    std::uint64_t total_writes = 0, publication_writes = 0, before_reference = 0, applied_reference = 0;
    std::uint64_t faults = 0, index_faults = 0, publisher_faults = 0, commit_faults = 0;
    // nth=0 is the real successful calibration. Every nth write in that exact
    // operation is then failed on a fresh identical store/cache history.
    for (std::uint64_t nth = 0; nth <= total_writes; ++nth) {
        Rig rig;
        ScratchStore store(rig.config(), &rig.backing);
        Subject subject(store);
        Model model{{0, ce::bits(0.1)}, {2, ce::bits(0.2)}, {UINT64_MAX, ce::bits(0.3)}};
        for (const auto& row : model) subject.edit(Action::insert, row.first, row.second);
        subject.compare(model);
        const auto before = digest(store);
        if (!nth) before_reference = before;
        else word(before, before_reference, "fault trial repeats exact calibrated starting state");
        ok(store.begin());
        const auto writes = rig.backing.writes;
        if (nth) rig.backing.arm(nth);
        bool failed = false;
        unsigned stage = 0;
        try {
            const auto edit = subject.pending(action, action == Action::insert ? 1 : 2, ce::bits(0.75));
            stage = 1;
            subject.publish(edit);
            if (!nth) publication_writes = rig.backing.writes - writes;
            stage = 2;
            pineforge::detail::working_store::check(store.commit(ScratchDurability::buffered));
        } catch (const ScratchIoError&) { failed = true; }
        rig.backing.disarm();
        auto applied = model;
        if (action == Action::insert) applied[1] = ce::bits(0.75);
        else applied.erase(2);
        if (!nth) {
            require(!failed, "successful fault calibration");
            total_writes = rig.backing.writes - writes;
            require(publication_writes && total_writes > publication_writes,
                    "calibration covers edits, publisher and final commit writes");
            applied_reference = digest(store);
            require(applied_reference != before_reference, "edit changes real persisted bytes");
            subject.compare(applied);
            continue;
        }
        require(failed && rig.backing.fired, "every nth write actually reached and failed");
        require(store.transaction_active(), "failed work remains available for explicit rollback");
        if (stage == 0) {
            ++index_faults;
            require(subject.index.recovery_required(), "index failure poisons all provisional frames");
            bool refused = false;
            try { (void)subject.index.answer({0, 0, 1, cohort, 1}); }
            catch (const Failure& error) { refused = error.reason == Reason::recovery_required; }
            require(refused, "poisoned attachment refuses reads until rollback and attach");
        } else if (stage == 1) ++publisher_faults;
        else ++commit_faults;
        ok(store.rollback());
        word(digest(store), before, "rollback restores full state digest, including handles and summaries");
        subject.recover();
        subject.compare(model);
        subject.edit(action, action == Action::insert ? 1 : 2, ce::bits(0.75));
        subject.compare(applied);
        word(digest(store), applied_reference, "same edit succeeds identically after rollback plus attach");
        ++faults;
    }
    require(faults == total_writes && index_faults && publisher_faults && commit_faults,
            "entire calibrated write set tested, including both publication boundaries");
    std::printf("recovery op=%s writes=%llu faults=%llu index=%llu publisher=%llu commit=%llu\n",
        action == Action::insert ? "insert" : "erase",
        static_cast<unsigned long long>(total_writes), static_cast<unsigned long long>(faults),
        static_cast<unsigned long long>(index_faults), static_cast<unsigned long long>(publisher_faults),
        static_cast<unsigned long long>(commit_faults));
}
void cohort_index_recovery() {
    fault_sweep(Action::insert);
    fault_sweep(Action::erase);
    Rig rig;
    std::uint64_t before = 0, baseline_records = 0;
    Model model;
    for (std::uint64_t i = 0; i < 65; ++i) model[i * 4] = ce::bits(0.125);
    {
        ScratchStore store(rig.config(), &rig.backing);
        Subject subject(store);
        baseline_records = store.record_count();
        for (const auto& row : model) subject.edit(Action::insert, row.first, row.second);
        subject.compare(model);
        require(store.record_count() == baseline_records + 2 * model.size() - 1,
                "exactly 2V-1 combined node records, no auxiliary allocator");
        before = digest(store);
        // Also discard a completely successful provisional edit after an
        // outer rollback: the next entry must reload the restored descriptor.
        ok(store.begin());
        subject.publish(subject.pending(Action::replace, 0, ce::bits(100.0)));
        ok(store.rollback());
        subject.recover();
        word(digest(store), before, "outer rollback discards successful provisional root and answer");
        subject.compare(model);
    }
    {
        ScratchStore store(rig.config(ScratchOpenMode::reopen), &rig.backing);
        const auto writes = store.stats().page_writes;
        Subject subject(store, false);
        subject.compare(model);
        word(digest(store), before, "read-only reopen keeps the complete digest");
        require(store.stats().page_writes == writes, "attach, validation and answer perform no writes");
        ok(store.begin());
        auto live = subject.current();
        auto detached = subject.index.detach(live);
        require(subject.index.metrics().loads == 0 && subject.index.metrics().puts == 0,
                "detach touches zero tree nodes");
        subject.publish({live, 0, 0, 0});
        ok(store.commit(ScratchDurability::buffered));
        word(subject.index.answer(subject.current()), 0, "detached cohort observes +0 immediately");
        // The same generation can contain detached and new live nodes.
        // Allocation must include the detached keys, not reuse their IDs.
        const auto detached_records = store.record_count();
        subject.edit(Action::insert, 0, ce::bits(0.75));
        require(store.record_count() == detached_records + 1,
                "new tree in same generation cannot overwrite a detached node");
        subject.compare(Model{{0, ce::bits(0.75)}});
        subject.index.validate(detached);
        subject.edit(Action::erase, 0);
        require(store.record_count() == detached_records, "new live tree erased independently of detached tree");
        // A provisional reclaim cursor is a plain transaction output. Discard
        // it on outer rollback, reattach, and resume from the prior cursor.
        const auto pre_reclaim = digest(store);
        auto provisional = detached;
        ok(store.begin());
        (void)subject.index.reclaim_step(provisional);
        ok(store.rollback());
        subject.recover();
        word(digest(store), pre_reclaim, "reclamation splice and erases roll back together");
        subject.index.validate(detached);
        std::uint64_t steps = 0;
        while (detached.root) {
            const auto remaining = detached.count;
            const auto records = store.record_count();
            const auto stats = store.stats();
            ok(store.begin());
            const auto done = subject.index.reclaim_step(detached);
            const auto metrics = subject.index.metrics();
            ok(store.commit(ScratchDurability::buffered));
            ++steps;
            // h+1 descent + parent + grandparent = h+3 <=67 direct loads.
            // Two erases and <=1 put use E2 Q2's 4096*(H+1) allowance;
            // this fresh 65-leaf tree has H<=ceil(log2 65)+3 =10.
            constexpr std::uint64_t reclaim_page_bound = 64 * 67 + 3 * 4096 * 11 + 4096;
            require(metrics.loads <= 67 && metrics.puts <= 1, "one bounded reclamation splice");
            require(store.stats().page_reads - stats.page_reads <= reclaim_page_bound,
                    "reclamation's backing-load ceiling is derived from fixed geometry");
            require(detached.count + 1 == remaining && done == (detached.count == 0),
                    "one remaining leaf retired per step");
            require(store.record_count() + (remaining == 1 ? 1 : 2) == records,
                    "leaf and redundant parent are actually freed");
            require(store.record_count() == baseline_records
                        + (detached.count ? 2 * detached.count - 1 : 0),
                    "no lost detached subtree or leaked record");
            require(steps <= model.size(), "reclamation completes in at most V steps");
        }
        require(steps == model.size() && store.record_count() == baseline_records,
                "every combined node reclaimed exactly once");
        subject.index.validate(subject.current());
        std::printf("reclaim V=%zu steps=%llu records=%llu baseline=%llu\n", model.size(),
            static_cast<unsigned long long>(steps),
            static_cast<unsigned long long>(store.record_count()),
            static_cast<unsigned long long>(baseline_records));
    }
}

void cohort_index_refusals() {
    Rig rig;
    ScratchStore store(rig.config(), &rig.backing);
    Subject subject(store);
    Model model{{0, ce::bits(1.0)}, {UINT64_MAX, ce::bits(0.5)}};
    for (const auto& row : model) subject.edit(Action::insert, row.first, row.second);
    const auto before = digest(store);
    for (const auto action : {Action::insert, Action::replace}) {
        ok(store.begin());
        bool refused = false;
        try { (void)subject.pending(action, action == Action::insert ? 0 : 17, ce::bits(2.0)); }
        catch (const Failure& error) { refused = error.reason == Reason::invalid_state; }
        require(refused && subject.index.recovery_required(),
                "duplicate insertion / absent replacement is a typed invariant refusal");
        ok(store.rollback());
        subject.recover();
        word(digest(store), before, "refused edit preserves original bytes after explicit recovery");
        subject.compare(model);
    }
    const auto root = subject.current();
    const auto no_op_writes = store.stats().page_writes;
    const auto absent_edit = subject.index.erase(root, 17);
    require(absent_edit.root.root == root.root && absent_edit.root.count == root.count
                && absent_edit.puts == 0, "absent erase is an unchanged root and zero node puts");
    word(absent_edit.answer_bits, oracle(model), "absent erase returns current answer");
    require(store.stats().page_writes == no_op_writes && !store.transaction_active(),
            "standalone absent erase is read-only, including transaction metadata");
    word(digest(store), before, "absent erase is a logical no-op");
    // Corrupt through the public store seam, preserving the key and handle.
    // A flipped format byte must throw the typed invariant failure, not zero.
    std::array<std::byte, 64> key{};
    std::array<std::byte, Index::node_bytes> value{};
    ok(store.read(root.root, ScratchField::key, 0, {key.data(), key.size()}));
    ok(store.read(root.root, ScratchField::value, 0, {value.data(), value.size()}));
    value[0] ^= std::byte{0x40};
    ok(store.begin());
    ScratchSpanSource key_source({key.data(), key.size()}), value_source({value.data(), value.size()});
    ScratchHandle replaced = 0;
    ok(store.put(key_source, value_source, replaced));
    require(replaced == root.root, "corruption targets the actual root record");
    bool invariant = false;
    try { (void)subject.index.answer(root); }
    catch (const Failure& error) { invariant = error.reason == Reason::invalid_state; }
    require(invariant && subject.index.recovery_required(), "corrupt node never becomes a wrong/zero answer");
    ok(store.rollback());
    subject.recover();
    word(digest(store), before, "rollback recovers the original valid node bytes");
    subject.compare(model);
}

struct Test { const char* name; void (*body)(); };
constexpr Test tests[] = {
    {"cohort_index_differential", &cohort_index_differential},
    {"cohort_index_exposure_candidate", &cohort_index_exposure_candidate},
    {"cohort_index_cost", &cohort_index_cost},
    {"cohort_index_recovery", &cohort_index_recovery},
    {"cohort_index_refusals", &cohort_index_refusals},
};
} // namespace

int main(int argc, char** argv) {
    global_allocation::recording = true;
    try {
        ce::require_round_to_nearest();
        const std::string selected = argc > 1 ? argv[1] : "";
        std::size_t rows = 0;
        for (const auto& test : tests) {
            if (!selected.empty() && selected != test.name) continue;
            const auto before = checkpoints;
            test.body();
            require(checkpoints > before, "selected row executed nonzero real checkpoints");
            ++rows;
            std::printf("case=%s checkpoints=%zu PASS\n", test.name, checkpoints - before);
        }
        require(rows != 0 && checkpoints != 0, "no matching row/checkpoints is a failure");
        std::printf("cohort exposure index: rows=%zu checkpoints=%zu PASS\n", rows, checkpoints);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "cohort exposure index: checkpoints=%zu FAIL: %s\n", checkpoints, error.what());
        return 1;
    }
}
