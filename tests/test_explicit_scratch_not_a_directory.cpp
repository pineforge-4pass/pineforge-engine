// Linux devtmpfs/tmpfs paths expose the path-statfs regression without mounts.
#include "scratch_invalid_directory_fixture.hpp"
#include "../src/report_scratch.hpp"

namespace {
void row() {
    using namespace s4e_tmp;
    PlainEnvironment environment;
    InvalidDirectories directories;
    directories.each([](const std::string& path, int number) {
        for (const bool bounded : {true, false}) {
            pineforge::detail::ScratchFile file;
            if (bounded) file.bound_cache();
            file.set_directory(path);
            expect_directory_error([&] { file.open("pineforge-invalid-explicit"); }, number,
                                   pineforge::RunFailureCode::report_sink_failed, true);
        }
        expect_directory_error([&] { pineforge::report_sink::ScratchFile file(path); }, number,
                               pineforge::RunFailureCode::report_sink_failed, true);
        Environment command("PINEFORGE_COMMAND_SCRATCH_DIR", path.c_str());
        expect_directory_error([] { (void)pineforge::detail::make_command_scratch_backing(4096); },
                               number, pineforge::RunFailureCode::scratch_io_failed, false);
    });
}
} // namespace

int main() {
    return s4e_tmp::test_main("test_explicit_scratch_not_a_directory", row);
}
