// Twin row of test_broker_state_hash_spilled_read_failure: the Pine host's
// three readers of the fold (F-03, round 5): its projection, the continuation
// capture and the scheduler's row (see the Pine part of the body). The body
// includes the source layer only under PINEFORGE_B1_PINE_HOST, so this row is
// registered only when the source layer is built and the other rows stay
// source-free.
#define PINEFORGE_B1_PINE_HOST
#include "test_broker_state_hash_spilled_read_failure.cpp"
