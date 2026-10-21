#pragma once
#include <pineforge/source/market_admission.hpp>
#include <map>
namespace pineforge::compat::pine {
bool explicit_pair_scope(const admission::Configuration& c);
bool default_gross_scope(const admission::Configuration& c);
bool original_pair_call(const admission::CommandObservation& o);
bool original_default_call(const admission::CommandObservation& o);
bool opening_qualification(const admission::Draft& draft);
bool explicit_qualification(const admission::Draft& draft);
bool awaits_pair_review(const admission::Draft& draft);
bool awaits_default_review(const admission::Draft& draft);
struct History {
    // Non-flat diagnostic snapshot. Policy readers can query HistoryView one
    // bar at a time without constructing either transient map.
    std::map<int,uint64_t> pair_causes;
    std::map<int,uint64_t> default_causes;
};
int last_rejected_command_bar(const admission::Journal& journal);
History admission_history(const admission::Journal& journal);
class HistoryView {
public:
    explicit HistoryView(const admission::Journal& journal) noexcept : journal_(&journal) {}
    // Borrowed live view: a later query observes subsequent journal edits.
    // Zero means absent; every actual cause has a nonzero allocated sequence.
    uint64_t pair_cause(int bar) const;
    uint64_t default_cause(int bar) const;
private:
    uint64_t cause(int bar, bool pair) const;
    const admission::Journal* journal_;
};
inline HistoryView admission_history_view(const admission::Journal& journal) noexcept {
    return HistoryView(journal);
}
} // namespace pineforge::compat::pine
