#include "perf_nopartial_rows.hpp"
// A late callback fault must remove privately written T-bearing windows,
// preserve the public path, and publish no report summary.
// Batch ingress preserves the DESIGN.md path-sink property: the long trace
// forces T-bearing stream windows to disk during the run, before the late
// callback exception; the failed public path must still contain no prefix.
void perf_nopartial::stream_file() { failure_case(Fault::stream_file); }
