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
    // Transient fold: maps retain an actual causal event, never stored flags.
    std::map<int,uint64_t> pair_causes;
    std::map<int,uint64_t> default_causes;
};
int last_rejected_command_bar(const admission::Journal& journal);
History admission_history(const admission::Journal& journal);
} // namespace pineforge::compat::pine
