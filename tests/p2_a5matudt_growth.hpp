#pragma once

#include "p2_a5matudt_payload.hpp"
#include <pineforge/generic_matrix.hpp>
#include <pineforge/matrix.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <stdexcept>
#include <vector>
#if __has_include(<pineforge/detail/matrix_store.hpp>)
#include "p2_a5matudt_store.hpp"
#endif

namespace p2_matrix_growth {
// Store: 64 cache pages plus 6 work pages. One metadata page, four fixed
// graph/matrix buffers, fixed slot/bucket records and one chunk buffer are
// charged below, along with the family handle. No term depends on N.
inline constexpr std::int64_t page_bytes = 4096;
inline constexpr std::int64_t resident_pages = 64;
inline constexpr std::int64_t fixed_pages = 6 + 1 + 4;

inline std::int64_t heap_bytes() {
    const auto info = ::mallinfo2();
    return static_cast<std::int64_t>(info.uordblks)
        + static_cast<std::int64_t>(info.hblkhd);
}

inline void require(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}

struct Numeric {
    pineforge::NumericMatrix matrix = pineforge::NumericMatrix::new_(0, 1);
    std::vector<double> row{0};
    void append(int bar) {
        row[0] = static_cast<double>(bar) + 0.125;
        matrix.add_row(matrix.rows(), row);
    }
    void verify(int count) const {
        require(matrix.rows() == count && matrix.columns() == 1, "numeric population truncated");
        for (int i = 0; i < count; ++i)
            require(matrix.get(i, 0) == static_cast<double>(i) + 0.125, "numeric old cell changed");
    }
};

struct Generic {
    pineforge::GenericMatrix<P2MatrixPayload> matrix =
        pineforge::GenericMatrix<P2MatrixPayload>::new_(0, 1);
    std::vector<P2MatrixPayload> row{p2_matrix_payload(0)};
    void append(int bar) {
        row[0] = p2_matrix_payload(bar);
        matrix.add_row(matrix.rows(), row);
    }
    void verify(int count) const {
        require(matrix.rows() == count && matrix.columns() == 1, "UDT population truncated");
        for (int i = 0; i < count; ++i)
            require(p2_payload_equal(matrix.get(i, 0), p2_matrix_payload(i)),
                    "UDT old string/vector cell changed");
    }
};

struct Boolean {
    pineforge::GenericMatrix<bool> matrix = pineforge::GenericMatrix<bool>::new_(0, 4);
    std::vector<bool> row{false, false, false, false};
    void append(int bar) {
        for (int col = 0; col < 4; ++col) row[static_cast<std::size_t>(col)] = (bar + col) % 3 == 0;
        matrix.add_row(matrix.rows(), row);
    }
    void verify(int count) const {
        require(matrix.rows() == count && matrix.columns() == 4, "bool population truncated");
        for (int i = 0; i < count; ++i)
            for (int col = 0; col < 4; ++col)
                require(matrix.get(i, col) == ((i + col) % 3 == 0), "bool old cell changed");
    }
};

template<class Family>
class Host final : public pineforge::source::PineStrategyHost {
public:
    Family family;
    int callbacks = 0;
    std::int64_t family_heap = 0;
    std::int64_t peak_family_heap = 0;
    void on_source_bar(const pineforge::Bar&) override {
        const int index = pine_bar_index();
        require(index == callbacks, "growth needs exactly one evaluation per synthetic bar");
        // Isolate the family from the host's unrelated series/report state:
        // count NET retained libc heap across each real public matrix mutation.
        // mallinfo2 includes Eigen's malloc and mmap-backed large allocations.
        const auto before = heap_bytes();
        family.append(index);
        family_heap += heap_bytes() - before;
        if (family_heap > peak_family_heap) peak_family_heap = family_heap;
        ++callbacks;
    }
};

template<class Family>
int run(int count, const char* kind) {
    try {
        // The only N-sized test input is deterministic OHLCV, outside all
        // measured intervals. The host/kernel consumes it through run().
        std::vector<pineforge::Bar> bars;
        bars.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            const double open = 100.0 + static_cast<double>(i % 97) * 0.125;
            bars.push_back({open, open + 1, open - 1, open + 0.25, 1000,
                            INT64_C(1700000100000) + i * INT64_C(60000)});
        }
#if __has_include(<pineforge/detail/matrix_store.hpp>)
        p2_a5::StoreFixture external_store;
#endif
        Host<Family> host;
        host.run(bars.data(), count, "1", "1", false);
        require(host.last_error().empty(), host.last_error().c_str());
        require(host.callbacks == count, "kernel did not reach every bar");
#if __has_include(<pineforge/detail/matrix_store.hpp>)
        external_store.runtime.collect();
#endif
        host.family.verify(count); // late read of EVERY old element
        constexpr auto bound = (resident_pages + fixed_pages) * page_bytes
            + resident_pages * 32 + 128 * 4 + 1024 + 16
            + static_cast<std::int64_t>(sizeof(Family));
        std::printf("P2-A5 matrix kind=%s bars=%d retained_delta=%" PRId64
                    " peak_delta=%" PRId64 " bound=%" PRId64 "\n",
                    kind, count, host.family_heap, host.peak_family_heap, bound);
#if __has_include(<pineforge/detail/matrix_store.hpp>)
        const auto stats = external_store.store->stats();
        std::printf("P2-A5 matrix store kind=%s bars=%d live=%" PRIu64
                    " resident=%" PRIu64 " backing=%" PRIu64 " digest=%016" PRIx64 "\n",
                    kind, count, stats.bytes_live, stats.bytes_resident, stats.bytes_backing,
                    external_store.runtime.graph().hash_state());
#endif
        std::fflush(stdout);
        require(host.peak_family_heap <= bound, "matrix retained heap grows with bars");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "P2-A5 matrix %s: %s\n", kind, error.what());
        return 1;
    }
}
} // namespace p2_matrix_growth
