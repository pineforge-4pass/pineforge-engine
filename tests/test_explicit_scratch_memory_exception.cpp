// The unbounded report raiser must have the same typed refusal as bounded I/O.
#include "scratch_invalid_directory_fixture.hpp"
#include "../src/report_scratch.hpp"

namespace {
void row() {
    using namespace s4e_tmp;
    PlainEnvironment environment;
    require_filesystems(false);
    for (const bool bounded : {false, true}) {
        pineforge::detail::ScratchFile file;
        if (bounded) file.bound_cache();
        file.set_directory("/dev/shm");
        expect_directory_error([&] { file.open("pineforge-memory-type"); }, ENODEV,
                               pineforge::RunFailureCode::report_sink_failed, true);
    }
    expect_directory_error([] { pineforge::report_sink::ScratchFile file("/dev/shm"); }, ENODEV,
                           pineforge::RunFailureCode::report_sink_failed, true);
    pineforge::detail::ScratchFile auxiliary(
        pineforge::detail::ScratchFile::IoRaiser::auxiliary);
    auxiliary.set_directory("/dev/shm");
    expect_directory_error([&] { auxiliary.open("pineforge-memory-auxiliary"); }, ENODEV,
                           pineforge::RunFailureCode::scratch_io_failed, false);
}
} // namespace

int main() {
    return s4e_tmp::test_main("test_explicit_scratch_memory_exception", row);
}
