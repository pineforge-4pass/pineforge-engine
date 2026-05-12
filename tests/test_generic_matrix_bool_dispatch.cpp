// test_generic_matrix_bool_dispatch.cpp
// Targeted regression for typed-matrix-probe-01-bool-regime-mask bug:
// Engine produces 785 trades, TV 774. Matrix gating is a no-op in engine.
// Tests cover: 24x7 init, set/get round-trip, transpose round-trip,
// hotCount accumulation, sample == mTT.get(h,d) fidelity.

#include <pineforge/generic_matrix.hpp>
#include <cassert>
#include <cstdio>
#include <vector>
#include <set>
#include <utility>

using pineforge::PineGenericMatrix;

// ── T1: 24×7 init — all cells false ─────────────────────────────────────────
static void test_24x7_init_all_false() {
    auto mask = PineGenericMatrix<bool>::new_(24, 7, false);
    assert(mask.rows() == 24);
    assert(mask.columns() == 7);
    for (int h = 0; h < 24; ++h)
        for (int d = 0; d < 7; ++d)
            assert(mask.get(h, d) == false);
}

// ── T2: set (h,d) — only that cell becomes true ──────────────────────────────
static void test_set_single_cell_isolation() {
    auto mask = PineGenericMatrix<bool>::new_(24, 7, false);
    mask.set(5, 3, true);
    for (int h = 0; h < 24; ++h)
        for (int d = 0; d < 7; ++d) {
            bool expected = (h == 5 && d == 3);
            assert(mask.get(h, d) == expected);
        }
}

// ── T3: transpose dimensions ─────────────────────────────────────────────────
static void test_transpose_dimensions() {
    auto mask = PineGenericMatrix<bool>::new_(24, 7, false);
    auto mT = mask.transpose();
    assert(mT.rows() == 7);
    assert(mT.columns() == 24);
    auto mTT = mT.transpose();
    assert(mTT.rows() == 24);
    assert(mTT.columns() == 7);
}

// ── T4: round-trip transpose is identity (sparse mask) ───────────────────────
static void test_transpose_roundtrip_sparse() {
    auto mask = PineGenericMatrix<bool>::new_(24, 7, false);
    // Sparse population: some cells set
    std::vector<std::pair<int,int>> cells = {{0,0},{5,3},{7,6},{14,0},{23,6},{12,4}};
    for (auto [h,d] : cells)
        mask.set(h, d, true);

    auto mTT = mask.transpose().transpose();
    assert(mTT.rows() == 24);
    assert(mTT.columns() == 7);
    for (int h = 0; h < 24; ++h)
        for (int d = 0; d < 7; ++d)
            assert(mTT.get(h, d) == mask.get(h, d));
}

// ── T5: hotCount — count of true cells in mTT equals unique pairs set ─────────
static void test_hotcount_increments_by_one_per_unique_pair() {
    auto mask = PineGenericMatrix<bool>::new_(24, 7, false);
    std::set<std::pair<int,int>> seen;

    // Simulate bars: set a new (h,d) each iteration, verify hotCount
    // Some repeats to test idempotency
    std::vector<std::pair<int,int>> sequence = {
        {0,0},{1,1},{2,2},{3,3},{4,4},{5,5},{6,6},
        {7,0},{8,1},{9,2},{10,3},{11,4},{12,5},{13,6},
        // repeats:
        {0,0},{7,0},{1,1},
        // new:
        {14,0},{15,1},{16,2},{17,3},{18,4},{19,5},{20,6}
    };

    for (auto [h,d] : sequence) {
        mask.set(h, d, true);
        seen.insert({h, d});

        // Re-compute mTT each bar (mimicking Pine's := reassignment)
        auto mT  = mask.transpose();
        auto mTT = mT.transpose();

        // hotCount via loop over mTT
        int hotCount = 0;
        for (int i = 0; i < 24; ++i)
            for (int j = 0; j < 7; ++j)
                if (mTT.get(i, j))
                    hotCount += 1;

        int expected_hot = static_cast<int>(seen.size());
        if (hotCount != expected_hot) {
            std::printf("FAIL T5: after setting (%d,%d): hotCount=%d expected=%d\n",
                        h, d, hotCount, expected_hot);
            assert(false);
        }
    }
}

// ── T6: sample = mTT.get(h,d) == mask.get(h,d) (round-trip value equality) ───
static void test_sample_equals_mask_get() {
    auto mask = PineGenericMatrix<bool>::new_(24, 7, false);
    mask.set(10, 2, true);
    mask.set(0,  0, true);
    mask.set(23, 6, true);

    auto mT  = mask.transpose();
    auto mTT = mT.transpose();

    for (int h = 0; h < 24; ++h)
        for (int d = 0; d < 7; ++d) {
            bool sample = mTT.get(h, d);
            bool direct = mask.get(h, d);
            if (sample != direct) {
                std::printf("FAIL T6: mTT.get(%d,%d)=%d != mask.get(%d,%d)=%d\n",
                            h, d, (int)sample, h, d, (int)direct);
                assert(false);
            }
        }
}

// ── T7: full 168-bar simulation mirroring tm-01 entry logic ──────────────────
// Each bar: set mask[h][d]=true if rsiVal>60, recompute mT/mTT,
// compute sample and hotCount, check entryCond gate behavior.
// Bug hypothesis: if mTT gating is a no-op, sample would be true
// even before mask[h][d] was set, letting extra trades through.
static void test_full_tm01_simulation() {
    auto mask = PineGenericMatrix<bool>::new_(24, 7, false);
    // Simulate: RSI>60 always (so mask gets set every bar)
    // Use a fixed sequence of (h,d) pairs covering all 168 unique slots
    int bars_with_entry = 0;
    int bars_total = 0;

    // First pass: we go through all 168 (h,d) pairs once.
    // On bar 0: mask is empty → hotCount=0 < 6 → no entry gate (correct)
    // After enough bars: hotCount>=6 and sample=true → gate allows entry (correct)
    // Bug: if transpose is wrong, sample might be true before set, allowing extra entries

    // Track first bar each (h,d) becomes true
    std::set<std::pair<int,int>> seen;

    for (int h = 0; h < 24; ++h) {
        for (int d = 0; d < 7; ++d) {
            ++bars_total;
            // Simulate: rsiVal > 60 fires → set mask
            mask.set(h, d, true);
            seen.insert({h, d});

            // Recompute mT, mTT each bar (Pine := semantics)
            auto mT  = mask.transpose();
            auto mTT = mT.transpose();

            // sample = mTT.get(h, d)
            bool sample = mTT.get(h, d);
            // hotCount
            int hotCount = 0;
            for (int i = 0; i < 24; ++i)
                for (int j = 0; j < 7; ++j)
                    if (mTT.get(i, j))
                        hotCount += 1;

            // Correctness: sample must be true (we just set it)
            if (!sample) {
                std::printf("FAIL T7: bar h=%d d=%d: sample=false after set — transpose bug!\n",
                            h, d);
                assert(false);
            }
            // hotCount must equal seen.size()
            if (hotCount != (int)seen.size()) {
                std::printf("FAIL T7: bar h=%d d=%d: hotCount=%d expected=%d — transpose bug!\n",
                            h, d, hotCount, (int)seen.size());
                assert(false);
            }

            // entryCond: hotCount >= 6 AND sample
            bool entryCond = (hotCount >= 6) && sample;
            if (entryCond) ++bars_with_entry;
        }
    }

    // All 168 unique cells visited. hotCount at end must be 168.
    auto mTT_final = mask.transpose().transpose();
    int final_hot = 0;
    for (int i = 0; i < 24; ++i)
        for (int j = 0; j < 7; ++j)
            if (mTT_final.get(i, j)) ++final_hot;
    assert(final_hot == 168);
    std::printf("  T7: bars_total=%d, bars_with_entryCond=%d, final_hotCount=%d\n",
                bars_total, bars_with_entry, final_hot);
}

// ── T8: test that a freshly-initialized mTT (before any set) has all false ───
// This tests a subtle bug: if transpose() on an all-false matrix somehow
// produces non-zero values in the transposed data.
static void test_transpose_of_false_matrix_is_false() {
    auto mask = PineGenericMatrix<bool>::new_(24, 7, false);
    auto mT   = mask.transpose();  // 7x24
    auto mTT  = mT.transpose();    // 24x7

    assert(mT.rows() == 7 && mT.columns() == 24);
    assert(mTT.rows() == 24 && mTT.columns() == 7);

    for (int i = 0; i < 7; ++i)
        for (int j = 0; j < 24; ++j)
            assert(mT.get(i, j) == false);

    for (int h = 0; h < 24; ++h)
        for (int d = 0; d < 7; ++d)
            assert(mTT.get(h, d) == false);
}

// ── T9: test pre-set sample — the KEY bug probe ──────────────────────────────
// Before mask[h][d] is set, mTT.get(h,d) must be FALSE.
// If it returns TRUE, the gate is bypassed → extra trades.
static void test_sample_false_before_set() {
    auto mask = PineGenericMatrix<bool>::new_(24, 7, false);

    // Bar 0: nothing set yet. For any (h,d), sample must be false.
    for (int h = 0; h < 24; ++h) {
        for (int d = 0; d < 7; ++d) {
            auto mT  = mask.transpose();
            auto mTT = mT.transpose();
            bool sample = mTT.get(h, d);
            if (sample) {
                std::printf("FAIL T9: mTT.get(%d,%d)=true before any set — BUG!\n", h, d);
                assert(false);
            }
        }
    }

    // Set only (5, 3). Then mTT.get(h,d) must be false for all (h,d) != (5,3).
    mask.set(5, 3, true);
    {
        auto mT  = mask.transpose();
        auto mTT = mT.transpose();
        for (int h = 0; h < 24; ++h) {
            for (int d = 0; d < 7; ++d) {
                bool sample  = mTT.get(h, d);
                bool expected = (h == 5 && d == 3);
                if (sample != expected) {
                    std::printf("FAIL T9: after set(5,3): mTT.get(%d,%d)=%d expected=%d\n",
                                h, d, (int)sample, (int)expected);
                    assert(false);
                }
            }
        }
    }
}

// ── T10: var-like semantics — mT/mTT reassigned each bar ────────────────────
// Mimics Pine `var matrix<bool> mT = ...; mT := transpose(mask)` per bar.
// The key: reassignment should always reflect the CURRENT state of mask.
static void test_var_reassign_reflects_current_mask() {
    auto mask = PineGenericMatrix<bool>::new_(24, 7, false);
    // Simulate var mT and var mTT as persistent objects, reassigned each bar
    auto mT  = PineGenericMatrix<bool>::new_(7, 24, false);
    auto mTT = PineGenericMatrix<bool>::new_(24, 7, false);

    // Bar 1: set (3, 2), reassign mT/mTT
    mask.set(3, 2, true);
    mT  = mask.transpose();
    mTT = mT.transpose();

    assert(mTT.get(3, 2) == true);
    assert(mTT.get(0, 0) == false);

    // Bar 2: set (10, 5), reassign mT/mTT
    mask.set(10, 5, true);
    mT  = mask.transpose();
    mTT = mT.transpose();

    assert(mTT.get(3,  2) == true);
    assert(mTT.get(10, 5) == true);
    assert(mTT.get(0,  0) == false);
    int hotCount = 0;
    for (int i = 0; i < 24; ++i)
        for (int j = 0; j < 7; ++j)
            if (mTT.get(i, j)) ++hotCount;
    assert(hotCount == 2);

    // Bar 3: no new set — just reassign. hotCount must still be 2.
    mT  = mask.transpose();
    mTT = mT.transpose();
    hotCount = 0;
    for (int i = 0; i < 24; ++i)
        for (int j = 0; j < 7; ++j)
            if (mTT.get(i, j)) ++hotCount;
    assert(hotCount == 2);
}

int main() {
    test_24x7_init_all_false();
    std::printf("T1 pass: 24x7 init all false\n");

    test_set_single_cell_isolation();
    std::printf("T2 pass: set single cell isolation\n");

    test_transpose_dimensions();
    std::printf("T3 pass: transpose dimensions\n");

    test_transpose_roundtrip_sparse();
    std::printf("T4 pass: transpose round-trip sparse\n");

    test_hotcount_increments_by_one_per_unique_pair();
    std::printf("T5 pass: hotCount increments by 1 per unique pair\n");

    test_sample_equals_mask_get();
    std::printf("T6 pass: sample == mask.get round-trip\n");

    test_full_tm01_simulation();
    std::printf("T7 pass: full 168-bar tm-01 simulation\n");

    test_transpose_of_false_matrix_is_false();
    std::printf("T8 pass: transpose of all-false matrix is all-false\n");

    test_sample_false_before_set();
    std::printf("T9 pass: sample false before set\n");

    test_var_reassign_reflects_current_mask();
    std::printf("T10 pass: var reassign reflects current mask\n");

    std::printf("All test_generic_matrix_bool_dispatch tests passed.\n");
    return 0;
}
