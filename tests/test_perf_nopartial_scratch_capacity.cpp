#include "perf_nopartial_rows.hpp"
// RED on b7e16f560: a real bounded cohort refusal leaves report rows readable.
// This is a bounds refusal, not an I/O error: no failed syscall can witness it.
void perf_nopartial::scratch_capacity() { failure_case(Fault::capacity); }
