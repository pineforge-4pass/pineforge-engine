#include "perf_nopartial_rows.hpp"
// RED on b7e16f560: a real ExitScratchRows scope-release fault publishes its prefix.
void perf_nopartial::scratch_teardown() { failure_case(Fault::teardown); }
