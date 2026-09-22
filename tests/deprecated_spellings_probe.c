/* R5 lane F6 item 4: a pure-C consumer of the P2c alias in pf_equity_stats_t.
 * tests/deprecated_spellings.cmake compiles it -fsyntax-only three ways; see
 * that script. PF_PROBE_HISTORICAL selects the deprecated spelling. */
#include <pineforge/pineforge.h>

double deprecated_spellings_probe_c(const pf_equity_stats_t* stats) {
#ifdef PF_PROBE_HISTORICAL
    return stats->sharpe_tv + stats->sortino_tv;
#else
    return stats->sharpe_monthly + stats->sortino_monthly;
#endif
}
