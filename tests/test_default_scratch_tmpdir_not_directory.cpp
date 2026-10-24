// A query error for TMPDIR must stop, even when /var/tmp is disk-backed.
#include "scratch_invalid_directory_fixture.hpp"

namespace {
void row() {
    using namespace s4e_tmp;
    PlainEnvironment environment;
    InvalidDirectories directories;
    directories.each([](const std::string& path, int number) {
        Environment temporary("TMPDIR", path.c_str());
        pineforge::detail::ScratchFile file;
        file.bound_cache();
        expect_directory_error([&] { file.open("pineforge-invalid-default"); }, number,
                               pineforge::RunFailureCode::report_sink_failed, true);
        expect_directory_error([] { (void)pineforge::detail::make_command_scratch_backing(4096); },
                               number, pineforge::RunFailureCode::scratch_io_failed, false);
        Host host;
        run(host);
        require(host.last_run_status() != 0 && host.callbacks == 0
                    && std::string(pineforge::run_failure_code_of(host)) == "scratch_io_failed"
                    && host.last_error().find(
                        std::error_code(number, std::generic_category()).message())
                        != std::string::npos,
                "invalid TMPDIR must fail the native run at begin with its original errno");
    });
}
} // namespace

int main() {
    return s4e_tmp::test_main("test_default_scratch_tmpdir_not_directory", row);
}
