// Twin row of test_broker_state_hash_spilled_read_failure: the base engine's
// projection (F-03, round 4). A lane file names every row after a source of
// its own, so the twin is this executable: the same body, its flag fixed.
#define PINEFORGE_B1_TWIN_FLAG "--base-projection"
#include "test_broker_state_hash_spilled_read_failure.cpp"
