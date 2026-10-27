// Store admission limits met through Store-backed matrices: the caller handle
// pool and the per-Store live-object limit.
//
// Pool row. The caller holds every pool handle with explicit begin() calls.
// Matrix operations inside still run, because each joins the open transaction
// through Store::scope(). One more begin() throws TxPoolExhausted, takes no
// handle and changes neither the matrices nor the caller's transaction, which
// keeps working and commits every joined change; the next returned slot is
// taken again.
//
// Limit row. At a small configured live-object limit (one Store object per
// matrix), matrix creation is admitted exactly up to the limit (CAP). The next
// object (CAP+1) throws ScratchCapacityError with the cohort argument from a
// lone operation of each element family, from a copy joined to a caller
// transaction (which becomes rollback-only) and in the middle of a sort, whose
// row permutation is admitted before its frozen copy is refused. Each refusal
// leaves the Store and every matrix exactly as before it and consumes no id;
// with room again, a copy reaches CAP and a sort that fits is admitted.
//
// Every expected count comes from the explicit begins and the creations made
// here, never from a run; every expected cell from the independent model.
//
// FAIL-BEFORE. The merge base's o1_store.hpp (3ea5a0d6) declares neither
// TxPoolExhausted, Store::scope() nor Shape::max_live_objects, and the Store
// test double this unit includes overrides hooks that header does not declare,
// so this unit does not compile against that header closure (a feature
// diagnostic). Behavioural RED: the handle-pool and live-object-limit mutation
// patches.
#include "a5m_store_fixture.hpp"
#include "a5m_reference.hpp"
#include <pineforge/detail/matrix_store.hpp>
#include <pineforge/detail/scratch_errors.hpp>
#include <pineforge/generic_matrix.hpp>
#include <pineforge/matrix.hpp>
#include <pineforge/run_failure.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace ms = pineforge::detail::matrix_store;
using a5m_test::require;
using pineforge::detail::ScratchCapacityError;
using pineforge::detail::o1::kMaxTxHandles;
using pineforge::detail::o1::RollbackOnly;
using pineforge::detail::o1::Store;
using pineforge::detail::o1::Tx;
using pineforge::detail::o1::TxPoolExhausted;
using Model = a5m_reference::Numeric;

// o1_store.hpp: with no transaction open, an operation owns one handle.
constexpr std::uint64_t kLoneOperationHandles = 1;

// The matrix and the model agree on the shape and on every cell, bit for bit.
void same_as(const pineforge::NumericMatrix& matrix, const Model& model, const char* why) {
    require(matrix.rows() == model.rows() && matrix.columns() == model.columns(), why);
    for (int r = 0; r < model.rows(); ++r)
        for (int c = 0; c < model.columns(); ++c) {
            const double actual = matrix.get(r, c), expected = model.get(r, c);
            require(std::memcmp(&actual, &expected, sizeof actual) == 0, why);
        }
}

void fifth_nested_begin_refused() {
    a5m_test::StoreFixture fixture;
    auto& counted = fixture.counted;
    const Store& pool = *fixture.store; // the real handle pool, read through no wrapper
    ms::Runtime runtime(counted);
    ms::ScopedRuntime scope(runtime);
    {
        // A lone operation (no transaction open) owns exactly one handle, and
        // what it reaches inside joins that one.
        auto matrix = pineforge::NumericMatrix::new_(3, 2);
        require(pool.stats().max_live_handles == kLoneOperationHandles &&
                    counted.begins() == kLoneOperationHandles && counted.live_handles() == 0,
                "a lone matrix operation did not own exactly one handle");
        auto model = Model::new_(3, 2);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 2; ++c) {
                const double value = (r * 5 + c * 3) % 7 + 0.5;
                matrix.set(r, c, value);
                model.set(r, c, value);
            }

        // The caller takes every pool handle: one outer scope, the rest joined.
        // caller_handles counts the explicit begins the caller holds at once.
        std::array<std::unique_ptr<Tx>, kMaxTxHandles> handles{};
        std::uint64_t caller_handles = 0;
        for (auto& handle : handles) {
            handle = counted.begin();
            ++caller_handles;
        }
        require(caller_handles == Store::max_caller_handles() &&
                    pool.stats().max_live_handles == caller_handles &&
                    counted.live_handles() == caller_handles,
                "the caller does not hold every pool handle");
        // With the pool full a matrix operation must join the caller's
        // transaction: no begin(), at least one joined scope, no new handle.
        const auto joined_operation = [&](auto&& call) {
            const auto joins = counted.joined_scopes();
            const auto begins = counted.begins();
            try {
                call();
            } catch (const TxPoolExhausted&) {
                require(false, "a matrix operation took a handle from the full caller pool");
            }
            require(counted.joined_scopes() > joins && counted.begins() == begins &&
                        pool.stats().max_live_handles == caller_handles &&
                        counted.live_handles() == caller_handles,
                    "a matrix operation did not join the caller transaction");
        };
        pineforge::NumericMatrix copy;
        joined_operation([&] { matrix.add_row(1, {9.25, -4.0}); });
        model.add_row(1, {9.25, -4.0});
        joined_operation([&] { copy = matrix.copy(); });
        auto copy_model = model.copy();
        joined_operation([&] { matrix.sort(0, true); });
        model.sort(0, true);

        // One more nested begin() is refused before it changes anything.
        const auto hash = matrix.external_hash(), copy_hash = copy.external_hash();
        const auto before = pool.stats();
        const auto begins = counted.begins();
        bool exhausted = false;
        try {
            auto fifth = counted.begin();
        } catch (const TxPoolExhausted&) {
            exhausted = true;
        }
        require(exhausted, "a nested explicit begin() beyond the caller pool was admitted");
        const auto after = pool.stats();
        require(matrix.external_hash() == hash && copy.external_hash() == copy_hash &&
                    after.bytes_live == before.bytes_live &&
                    after.objects_live == before.objects_live,
                "the refused begin() changed a matrix or the Store");
        require(after.max_live_handles == caller_handles &&
                    counted.live_handles() == caller_handles && counted.begins() == begins,
                "the refused begin() took or kept a handle");

        // The caller's transaction stays usable: more joined work, then the
        // innermost handle returns its slot and the next begin() takes it again.
        try {
            joined_operation([&] { matrix.remove_row(0); });
            model.remove_row(0);
            joined_operation([&] { copy.fill(-1.5); });
            copy_model = Model::new_(copy_model.rows(), copy_model.columns(), -1.5);
            handles.back()->commit();
            handles.back().reset();
            require(counted.live_handles() == caller_handles - 1,
                    "the innermost caller handle did not return its slot");
            handles.back() = counted.begin();
            require(pool.stats().max_live_handles == caller_handles &&
                        counted.live_handles() == caller_handles,
                    "the refused begin() consumed a pool slot");
            for (std::size_t i = handles.size(); i-- > 1;) handles[i]->commit();
            handles.front()->commit();
        } catch (const RollbackOnly&) {
            require(false, "the refused begin() made the caller transaction rollback-only");
        }
        for (auto& handle : handles) handle.reset();
        require(counted.live_handles() == 0 && counted.abandoned_scopes() == 0 &&
                    pool.stats().max_live_handles == caller_handles,
                "the pool row left a handle live or exceeded the explicit begins");
        // The commit kept every joined change.
        same_as(matrix, model, "the caller commit lost a joined matrix change");
        same_as(copy, copy_model, "the caller commit lost the joined copy");
        std::printf("matrix store limits: pool=%zu caller_handles=%llu max_live_handles=%llu "
                    "fifth begin refused\n",
                    Store::max_caller_handles(), static_cast<unsigned long long>(caller_handles),
                    static_cast<unsigned long long>(pool.stats().max_live_handles));
    }
    require(pool.stats().objects_live == 0 && pool.stats().bytes_live == 0,
            "the pool row retained Store objects");
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
           ", approved maximum " + grouped(pineforge::detail::o1::kMaxLiveObjectsCap);
}

// `call` must try to create one object past the limit. It must throw the
// Store's typed cohort refusal with that limit's text, and leave the Store's
// live counts and every matrix `state` hashes as they were.
template<class State, class Call>
void refused_at_limit(const Store& real, std::uint64_t limit, const State& state,
                      const char* admitted_message, Call&& call) {
    const auto before = real.stats();
    const auto matrices = state();
    bool refused = false;
    try {
        call();
    } catch (const ScratchCapacityError& error) {
        refused = true;
        require(error.run_failure_code() == pineforge::RunFailureCode::scratch_capacity_exceeded &&
                    error.run_failure().args != nullptr &&
                    *error.run_failure().args == "{\"resource\":\"cohort\"}" &&
                    std::string(error.what()) == object_limit_text(limit),
                "CAP+1 refusal lost its cohort argument or its text");
    } catch (const std::exception&) {
        require(false, "CAP+1 refusal is not the Store's typed ScratchCapacityError");
    }
    require(refused, admitted_message);
    const auto after = real.stats();
    require(after.objects_live == before.objects_live && after.bytes_live == before.bytes_live &&
                state() == matrices,
            "CAP+1 refusal changed a matrix or the Store");
}

void live_object_cap() {
    // A small configured limit, far below the cap, keeps CAP and CAP+1 cheap.
    constexpr std::uint64_t kLimit = 8;
    auto shape = a5m_test::shape();
    shape.max_live_objects = kLimit;
    a5m_test::StoreFixture fixture(shape);
    auto& counted = fixture.counted;
    const Store& real = *fixture.store;
    ms::Runtime runtime(counted);
    ms::ScopedRuntime scope(runtime);
    // A Store-backed matrix is one Store object; `objects` counts the
    // creations admitted here.
    std::uint64_t objects = 0;
    require(real.stats().max_live_objects == kLimit && real.stats().objects_live == objects,
            "the Store did not take the configured live-object limit");
    {
        const auto admitted = [&](auto&& create) {
            decltype(create()) matrix;
            try {
                matrix = create();
            } catch (const ScratchCapacityError&) {
                require(false, "CAP: a matrix below the live-object limit was refused");
            }
            ++objects;
            require(matrix.is_external_storage() && real.stats().objects_live == objects,
                    "an admitted matrix miscounted the live objects");
            return matrix;
        };
        auto numeric = admitted([] { return pineforge::NumericMatrix::new_(2, 2); });
        auto integers = admitted([] { return pineforge::GenericMatrix<int>::new_(2, 3, 7); });
        auto flags = admitted([] { return pineforge::GenericMatrix<bool>::new_(1, 4, true); });
        auto numeric_model = Model::new_(2, 2);
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 2; ++c) {
                numeric.set(r, c, 4.0 - (r * 2 + c)); // rows in descending key order
                numeric_model.set(r, c, 4.0 - (r * 2 + c));
            }
        // CAP: one-cell fillers up to the limit, every one admitted.
        std::vector<pineforge::NumericMatrix> fillers;
        while (objects < kLimit)
            fillers.push_back(admitted([] { return pineforge::NumericMatrix::new_(1, 1); }));
        require(!fillers.empty() && real.stats().objects_live == kLimit,
                "CAP: the Store did not reach its live-object limit");
        const auto last_admitted = fillers.back().external_identity();
        // What a refusal must leave as it was: every matrix's id, shape and cells.
        const auto state = [&] {
            std::vector<std::uint64_t> hashes{numeric.external_hash(), integers.external_hash(),
                                              flags.external_hash()};
            for (const auto& filler : fillers) hashes.push_back(filler.external_hash());
            return hashes;
        };

        // CAP+1 from a lone operation of each element family: its own scope
        // rolls back.
        refused_at_limit(real, kLimit, state,
                         "CAP+1: a lone numeric matrix.new past the limit was admitted",
                         [] { (void)pineforge::NumericMatrix::new_(1, 1); });
        refused_at_limit(real, kLimit, state,
                         "CAP+1: a lone integer matrix.copy past the limit was admitted",
                         [&] { (void)integers.copy(); });
        refused_at_limit(real, kLimit, state,
                         "CAP+1: a lone boolean matrix.transpose past the limit was admitted",
                         [&] { (void)flags.transpose(); });

        // CAP+1 inside a caller transaction: the joined copy that saw the
        // refusal leaves the caller rollback-only, whose rollback drops its
        // earlier change.
        const auto at_cap = state();
        {
            auto outer = counted.begin();
            numeric.set(0, 0, -99.0);
            const auto abandoned = counted.abandoned_scopes();
            refused_at_limit(real, kLimit, state,
                             "CAP+1: a joined matrix.copy past the limit was admitted",
                             [&] { (void)numeric.copy(); });
            require(counted.abandoned_scopes() > abandoned,
                    "the joined scope that saw the refusal did not abandon the caller transaction");
            bool rollback_only = false;
            try {
                outer->commit();
            } catch (const RollbackOnly&) {
                rollback_only = true;
            }
            require(rollback_only,
                    "a refusal inside a joined matrix operation left the caller committable");
        }
        require(state() == at_cap && real.stats().objects_live == kLimit,
                "the refused caller transaction kept its speculative change");
        same_as(numeric, numeric_model, "the refused caller transaction kept its speculative cell");

        // Releasing a filler frees its slot. No refusal consumed an id, so the
        // next creation receives the id after the last admitted one.
        fillers.pop_back();
        --objects;
        require(real.stats().objects_live == objects, "a released matrix kept its Store object");
        fillers.push_back(admitted([] { return pineforge::NumericMatrix::new_(1, 1); }));
        require(fillers.back().external_identity() == last_admitted + 1,
                "a refused matrix creation consumed an id");

        // CAP+1 in the middle of a sort. sort() builds two objects, its row
        // permutation and then a frozen copy of the rows; with one slot free the
        // permutation is admitted and the copy is refused, and the sort's own
        // scope rolls both back.
        fillers.pop_back();
        --objects;
        require(kLimit - objects == 1, "the mid-sort refusal needs exactly one free slot");
        refused_at_limit(real, kLimit, state, "CAP+1: a sort past the limit was admitted",
                         [&] { numeric.sort(0, true); });
        same_as(numeric, numeric_model, "the refused sort changed the matrix");

        // CAP again: the free slot still admits exactly one object.
        auto copy = admitted([&] { return numeric.copy(); });
        require(objects == kLimit, "CAP: the copy after the refusals did not reach the limit");
        same_as(copy, numeric_model, "the copy admitted at CAP is not exact");
        // With the copy and one filler released, two slots are free. The refusal
        // above showed the sort needs more than one object; admitted here, it
        // needs at most two, so its workspace reaches CAP exactly and is released
        // when it returns.
        copy = pineforge::NumericMatrix{};
        fillers.pop_back();
        objects -= 2;
        require(real.stats().objects_live == objects, "a released matrix kept its Store object");
        try {
            numeric.sort(0, true);
        } catch (const ScratchCapacityError&) {
            require(false, "CAP: a sort that fits exactly was refused");
        }
        numeric_model.sort(0, true);
        same_as(numeric, numeric_model, "the sort admitted at CAP is not exact");
        require(real.stats().objects_live == objects,
                "the sort admitted at CAP kept a workspace object");
    }
    require(real.stats().objects_live == 0 && real.stats().bytes_live == 0,
            "the limit row retained Store objects");
    std::printf("matrix store limits: live_object_limit=%llu CAP admitted; CAP+1 refused lone, "
                "joined and mid-sort\n",
                static_cast<unsigned long long>(kLimit));
}

} // namespace

int main() {
    try {
        // The pool row runs first and never nears the live-object limit, so a
        // handle or joining regression fails its checks, while a limit or
        // atomicity regression passes it and fails the limit row.
        fifth_nested_begin_refused();
        live_object_cap();
        std::puts("matrix store limits: handle pool and live-object limit rows reached");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "matrix store limits: %s\n", error.what());
        return 1;
    }
}
