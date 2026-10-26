#include "perf_nopartial_rows.hpp"
// RED on b7e16f560: an actual strategy callback exception publishes partial rows.
void perf_nopartial::callback_exception() { failure_case(Fault::callback); }
