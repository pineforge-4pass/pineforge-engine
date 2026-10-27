// Expected wall time: 5-30 seconds Release on a 10-core host (unmeasured).
// Frozen-head red: 500/2,000 resident ANY credits at B=2,000/8,000.
// Live entry ids never exceed two; every fresh id fills and completely closes.
#include "cr307_g8b_fixture.hpp"

int main() { return cr307_g8b::history_main(false); }
