#include "cr257_io.hpp"

namespace cr257 {

void source_100k() {
    // This selectable row runs BOTH populations itself; it never relies on
    // another CTest's output, scheduling order, or a stored timing/count table.
    const auto small = source_run(kSmallN);
    const auto large = source_run(kLargeN);
    source_bounds(small);
    source_bounds(large);
    // Saturating subtraction expresses an upper bound on growth; a decrease
    // is allowed and must not become an unsigned-underflow failure.
    const Count total_growth = large.io.total.pread.calls > small.io.total.pread.calls
        ? large.io.total.pread.calls - small.io.total.pread.calls : 0;
    const Count page_growth = large.io.pages.pread.calls > small.io.pages.pread.calls
        ? large.io.pages.pread.calls - small.io.pages.pread.calls : 0;
    bound("command.total_pread_growth_100k_minus_10k", total_growth,
          large.context.extent_pages, large.context);
    bound("command.page_pread_growth_100k_minus_10k", page_growth,
          large.context.extent_pages, large.context);
}

} // namespace cr257
