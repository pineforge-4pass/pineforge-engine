#include "perf_nopartial_rows.hpp"
// GUARD: report_sink_failed from an in-run record write already empties ReportC.
void perf_nopartial::sink_record_guard() { failure_case(Fault::sink_record); }
