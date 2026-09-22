// R5 lane F6 item 4: a C++ consumer of the P2c aliases -- the two
// exit_legs::Domain enumerators and the pf_equity_stats_t fields.
// tests/deprecated_spellings.cmake compiles it -fsyntax-only three ways; see
// that script. PF_PROBE_HISTORICAL selects the deprecated spellings.
#include <pineforge/exit_leg_lifecycle.hpp>
#include <pineforge/pineforge.h>

int deprecated_spellings_probe_cpp(const pf_equity_stats_t& stats) {
    using pineforge::exit_legs::Domain;
#ifdef PF_PROBE_HISTORICAL
    return static_cast<int>(Domain::Coof) + static_cast<int>(Domain::MagnifierCoof)
        + static_cast<int>(stats.sharpe_tv + stats.sortino_tv);
#else
    return static_cast<int>(Domain::FillRecalc)
        + static_cast<int>(Domain::MagnifierFillRecalc)
        + static_cast<int>(stats.sharpe_monthly + stats.sortino_monthly);
#endif
}
