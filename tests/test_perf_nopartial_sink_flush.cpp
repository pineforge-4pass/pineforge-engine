#include "perf_nopartial_rows.hpp"
// GUARD: a finish-time sink fdatasync failure already clears owned report arrays.
void perf_nopartial::sink_flush_guard() { failure_case(Fault::sink_flush); }
