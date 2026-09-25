/* Lane REL10: the C probe tests/removed_spellings.cmake compiles
 * -fsyntax-only. With no PF_PROBE_* macro it names the 1.0 spellings of
 * pf_equity_stats_t's two monthly ratios; PF_PROBE_<name> names one removed
 * pre-1.0 spelling alone. */
#include <pineforge/pineforge.h>

double removed_spellings_probe_c(const pf_equity_stats_t* stats);

double removed_spellings_probe_c(const pf_equity_stats_t* stats) {
#if defined(PF_PROBE_sharpe_tv)
    return stats->sharpe_tv;
#elif defined(PF_PROBE_sortino_tv)
    return stats->sortino_tv;
#else
    return stats->sharpe_monthly + stats->sortino_monthly;
#endif
}
