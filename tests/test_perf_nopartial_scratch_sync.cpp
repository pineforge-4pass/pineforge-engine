#include "perf_nopartial_rows.hpp"
// RED on b7e16f560: a checked durable commit fault still permits partial reports.
void perf_nopartial::scratch_sync() { failure_case(Fault::sync); }
