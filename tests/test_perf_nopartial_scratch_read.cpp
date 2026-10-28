#include "perf_nopartial_rows.hpp"
// RED on b7e16f560: scratch_io_failed does not close the report publication gate.
void perf_nopartial::scratch_read() { failure_case(Fault::read); }
