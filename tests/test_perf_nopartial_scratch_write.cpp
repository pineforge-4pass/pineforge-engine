#include "perf_nopartial_rows.hpp"
// RED on b7e16f560: a failed command-page write still permits partial reports.
void perf_nopartial::scratch_write() { failure_case(Fault::write); }
