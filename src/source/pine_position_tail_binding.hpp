#pragma once

// All L8c / L6b-4 binding lives here. Reconcile this file when those lanes
// land; the footer codec and percentage-exit predicate have no dependency
// on their implementation details.
#include "pine_position_tail.hpp"
#include "../report_scratch.hpp"

#include <pineforge/engine.hpp>
#include <pineforge/run_failure.hpp>

#include <limits>
#include <stdexcept>

namespace pineforge::source::detail {

struct PositionTailBinding {
    // L6b-4: already-bound directory, positional binary I/O, no fallback.
    class File {
    public:
        explicit File(const std::string& directory) : file_(directory) {}
        void write(std::uint64_t offset, const void* bytes, std::size_t count) {
            file_.write(static_cast<std::int64_t>(offset), bytes, count);
        }
        void read(std::uint64_t offset, void* bytes, std::size_t count) const {
            file_.read(static_cast<std::int64_t>(offset), bytes, count);
        }
        void clear() { file_.truncate(0); }
    private:
        report_sink::ScratchFile file_;
    };

    // L8c: call once on the approved resident prefix BEFORE trades.seal(end).
    // A failure propagates before any row can be discarded.
    static void preserve(std::shared_ptr<PositionTailSpill>& spill,
                         const TradeLedger& trades, std::size_t end,
                         std::int64_t position_open, const std::string& directory) {
        if (end < trades.base() || end > trades.size()) {
            throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
                                          "invalid Pine position-tail seal prefix");
        }
        if (position_open == std::numeric_limits<std::int64_t>::min()) {
            spill.reset();
            return;
        }
        for (std::size_t i = trades.base(); i < end; ++i) {
            const Trade& trade = trades[i];
            if (trade.exit_time < position_open) {
                // A scan barrier cuts off all older rows even when exit
                // times are not monotone in global trade-row order.
                spill.reset();
            } else {
                if (!spill) spill = std::make_shared<PositionTailSpill>(directory);
                spill->append(trade);
            }
        }
    }
};

} // namespace pineforge::source::detail
