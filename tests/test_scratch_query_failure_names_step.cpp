// RED by reading at 8837a5d608: /dev/null is labelled "statfs failed".
#include "default_scratch_tmpfs_fixture.hpp"
#include <pineforge/detail/scratch_file.hpp>

namespace {
template<class Operation>
void expect_check_failure(Operation operation, const char* prefix,
                          pineforge::RunFailureCode code) {
    using namespace s4e_tmp;
    try {
        operation();
    } catch (const std::system_error& error) {
        const std::string text = error.what();
        require(error.code() == std::error_code(ENOTDIR, std::generic_category())
                    && pineforge::classify_run_failure(error).code == code,
                "non-directory TMPDIR must retain its errno and failure code");
        require(text.find(prefix) == 0
                    && text.find("TMPDIR '/dev/null': directory check failed (errno "
                                 + std::to_string(ENOTDIR) + ": "
                                 + std::error_code(ENOTDIR, std::generic_category()).message() + ")")
                        != std::string::npos
                    && text.find("statfs") == std::string::npos,
                "a non-directory must name the directory check, not statfs");
        return;
    }
    throw std::runtime_error("non-directory TMPDIR unexpectedly succeeded");
}

void row() {
    using namespace s4e_tmp;
    std::puts("S4E inspection path: /dev/null (directory metadata only)");
    PlainEnvironment environment;
    Environment temporary("TMPDIR", "/dev/null");
    pineforge::detail::ScratchFile file;
    expect_check_failure([&] { file.open("pineforge-query-step"); }, "scratch create",
                         pineforge::RunFailureCode::report_sink_failed);
    expect_check_failure([] { (void)pineforge::detail::make_command_scratch_backing(4096); },
                         "command scratch create", pineforge::RunFailureCode::scratch_io_failed);
}
} // namespace

int main() {
    return s4e_tmp::test_main("test_scratch_query_failure_names_step", row);
}
