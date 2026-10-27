// C3 / T5. Same generator and transcript as T3, all commands through the
// existing prepare/install path. A stale token must fail the actual run.
// Held decoded copies stay frozen across a t=0 watermark advance; fresh
// decodes project. The single-spill seed also keeps raw pointers within the
// eight-keep window and detects an illegally cleared ring (corrected P2-6).
#define PINEFORGE_DORMANT_BOOK_NO_MAIN
#include "test_native_dormant_book_transcript.cpp"

int main() {
    try {
        using namespace dormant_test;
        run_fast(0x12345678, true).print("tokens-fast-a");
        run_fast(0x9abcdef0, true).print("tokens-fast-b");
        run_fast(0x12345678, true, true, true).print("tokens-partial-failure");
        run_fast(0x12345678, true, true, false, true).print("tokens-spilled-grid");
        for (const std::uint64_t seed : {7, 18, 43, 70}) {
            run_mixed(seed, false, true).print("tokens-mixed-raw");
            run_mixed(seed, true, true).print("tokens-mixed-grid");
        }
        run_carried(true).print("tokens-carried");
        run_active(true).print("tokens-active-spill");
        run_overflow(true).print("tokens-mid-point-active-spill");
        run_ring_window(true).print("tokens-ring-keepalive");
        run_middle_spill(true).print("tokens-middle-spill");
        run_driver_matrix(true).print("tokens-driver-matrix");
        run_complete(true).print("tokens-complete-before-interior");
        run_trail(true).print("tokens-active-trail");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "test_native_dormant_book_tokens: %s\n", e.what());
        return 1;
    }
}
