// Twin row of test_native_match_spilled_book_snapshot: the spilled book alone
// (B1-01; the sanitizers profile's AddressSanitizer row for the freed-row
// read). A lane file names every row after a source of its own, so the twin
// is this executable: the same body, its flag fixed.
#define PINEFORGE_B1_TWIN_FLAG "--spilled-only"
#include "test_native_match_spilled_book_snapshot.cpp"
