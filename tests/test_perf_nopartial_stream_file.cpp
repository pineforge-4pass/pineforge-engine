#include "perf_nopartial_rows.hpp"
// RED on b7e16f560: a late callback fault leaves already-written T records
// at the configured public path, and fill_report still publishes its summary.
void perf_nopartial::stream_file() { failure_case(Fault::stream_file); }
