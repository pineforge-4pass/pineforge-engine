// Twin row of test_broker_state_hash_spilled_read_failure: a host that catches
// the hash's throw (F-03). A lane file names every row after a source of its
// own, so the twin is this executable: the same body, its flag fixed.
#define PINEFORGE_B1_TWIN_FLAG "--swallowed"
#include "test_broker_state_hash_spilled_read_failure.cpp"
