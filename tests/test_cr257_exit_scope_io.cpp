// Direct write-amplification witness: exactly ONE scope, so its final partial
// batch is included in ceil(records/B), independently of the engine-run
// question about rounding a population of many separate scopes.
#include "cr257_io.hpp"
#include "../src/command_scratch_production.hpp"
#include <pineforge/source/exit_scratch.hpp>

namespace cr257 {

void exit_scope_io() {
    constexpr Count records = pineforge::source::detail::kExitScopeEraseBatch + 1;
    const auto run = scope_erase_run(records);
    const Count batches = ceil_div(records, pineforge::source::detail::kExitScopeEraseBatch);
    const Count pages = batches * pineforge::detail::kPagesPerCommit;
    bound("scope.erase_commits", run.commits, batches, run.context);
    bound("scope.erase_pwrite_calls", run.io.total.pwrite.calls, pages, run.context);
    bound("scope.erase_pwrite_bytes", run.io.total.pwrite.bytes,
          pages * platform_page, run.context);
}

} // namespace cr257
