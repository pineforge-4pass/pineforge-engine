#include "perf_nopartial_rows.hpp"
// RED on b7e16f560: a non-sink typed failure still publishes accumulated rows.
// The typed error is raised by the production recorder's out-of-range slot check.
void perf_nopartial::typed_failure() { failure_case(Fault::typed); }
