// Lane REL10: the C++ probe tests/removed_spellings.cmake compiles
// -fsyntax-only. With no PF_PROBE_* macro it names the 1.0 spellings -- the
// two exit_legs::Domain enumerators and the pf_equity_stats_t fields;
// PF_PROBE_<name> names one removed pre-1.0 spelling alone.
#include <pineforge/exit_leg_lifecycle.hpp>
#include <pineforge/pineforge.h>

int removed_spellings_probe_cpp(const pf_equity_stats_t& stats) {
    using pineforge::exit_legs::Domain;
#if defined(PF_PROBE_Coof)
    return static_cast<int>(Domain::Coof);
#elif defined(PF_PROBE_MagnifierCoof)
    return static_cast<int>(Domain::MagnifierCoof);
#elif defined(PF_PROBE_sharpe_tv)
    return static_cast<int>(stats.sharpe_tv);
#elif defined(PF_PROBE_sortino_tv)
    return static_cast<int>(stats.sortino_tv);
#else
    return static_cast<int>(Domain::FillRecalc)
        + static_cast<int>(Domain::MagnifierFillRecalc)
        + static_cast<int>(stats.sharpe_monthly + stats.sortino_monthly);
#endif
}
