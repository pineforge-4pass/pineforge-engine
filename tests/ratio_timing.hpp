// The sizing every timing-ratio row shares: a ratio is read only off legs long
// enough to measure (R5 lane CI-FLAKE).
//
// A ratio row times two legs of one workload -- N and 4N bars, a read early
// and late in a run, ten and forty live requests -- and bounds their quotient.
// Clock and scheduler noise decide the quotient of legs a few milliseconds
// long, so RATIO-HARDEN gave every row a minimum leg time and a calibration
// that grows the workload (repeats of a fixed run, its bars, its queries or
// its reads) until a sample reaches the minimum. Eight of the nine rows then
// timed their legs afresh and failed when one came in under the minimum,
// which it can: a calibration stops anywhere from the minimum to twice it,
// and a later best of several samples can come in under the one that crossed
// it. Engine CI run 36194784147 timed 0.0964 s (macOS Debug,
// test_native_continuation_digest_tail) and 0.0991 s (macOS Release,
// test_native_bare_host_contracts) at sizes a sample of at least 0.1 s had
// chosen. A short leg is not a result, and no longer a failure: the row
// doubles the workload and times its legs again, until every leg reaches the
// minimum. (The ninth, test_adapter_live_state_scaling, already timed its
// legs until the small one reached it; its loop is this one now.) A row's
// own minimum checks now fail only where its growth cap stopped the doubling
// -- a workload whose time stopped growing with its size -- and every cap is
// at least four times the size an Apple M4 Max needs, the fastest machine
// this lane timed.
#pragma once

namespace ratio_timing {

// Doubles `scale` from its start while one sample of the workload at that
// scale -- the row's `sample(scale)`, in seconds -- is under `min_seconds`,
// and never past `max_scale`: a cheap first size, so that the legs are
// usually timed once.
template<class Sample>
int presized(int scale, int max_scale, double min_seconds, Sample&& sample) {
    while (scale <= max_scale / 2 && sample(scale) < min_seconds) scale *= 2;
    return scale;
}

// Times the row's legs at `scale` -- `time_legs(scale)` takes the row's own
// samples of every leg (best of N, in turn where the row alternates them) and
// returns the shortest leg's seconds -- and doubles `scale` until that leg
// reaches `min_seconds`, or `scale` reaches `max_scale`. On return `scale` is
// the size the legs were last timed at.
template<class TimeLegs>
void time_measurable_legs(int& scale, int max_scale, double min_seconds,
                          TimeLegs&& time_legs) {
    while (time_legs(scale) < min_seconds && scale <= max_scale / 2) scale *= 2;
}

} // namespace ratio_timing
