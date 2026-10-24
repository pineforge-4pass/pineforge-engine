#include "scratch_candidate_message_fixture.hpp"
#include <pineforge/detail/id_pair_store.hpp>

namespace {
void row() {
    using namespace s4e_tmp;
    PlainEnvironment environment;
    CandidateDirectory directory;
    Environment temporary("TMPDIR", directory.missing.c_str());
    pineforge::detail::IdPairStore<std::uint64_t> store;
    expect_candidate_failure<pineforge::detail::IdPairStoreError>(
        [&] { (void)store.insert_or_assign("candidate", 1); },
        "id table create", directory.missing, pineforge::RunFailureCode::scratch_io_failed);
    require(store.empty(), "failed id-pair create must publish no row");
}
} // namespace

int main() {
    return s4e_tmp::test_main("test_id_pair_scratch_query_names_candidate", row);
}
