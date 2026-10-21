#pragma once

// Source-only helpers shared by the admission window and the v4 extension.
// No kernel seam or object layout depends on these views.
#include <pineforge/engine.hpp>
#include <pineforge/source/market_admission.hpp>
#include <string_view>

namespace pineforge::admission {
inline namespace market_admission_v2 {

// The same canonical traversal as diagnostic reflection, written directly to
// the sink without materializing owning Fields.
void reflect(const Event& value, std::string_view path, BrokerStateHashSink& sink);

struct FoldWindow {
    const std::vector<Event>& window;
    std::size_t base;
    const Event& operator[](std::size_t index) const { return window[index - base]; }
    std::size_t end() const { return base + window.size(); }
};

// Repair a query cursor after insertion, retain, reset or journal replacement.
// A retired prefix supplies exactly the bytes of the events it replaced.
inline FoldWindow prepare_fold(const Journal& journal, std::size_t& count,
                               uint64_t& digest, uint64_t& last_sequence) {
    const auto& base = journal.retired_prefix();
    const FoldWindow events{journal.events(), base.count};
    if (count < base.count || count > events.end()
        || (count == base.count
            ? (last_sequence != base.last_sequence || digest != base.digest)
            : sequence(events[count - 1]) != last_sequence)) {
        count = base.count;
        digest = base.digest;
        last_sequence = base.last_sequence;
    }
    return events;
}

} // inline namespace market_admission_v2
} // namespace pineforge::admission
