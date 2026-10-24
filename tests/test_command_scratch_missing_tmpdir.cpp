// Native command scratch is created at begin; both refusals retain its prefix.
#include "default_scratch_tmpfs_fixture.hpp"
#include <system_error>

namespace {
using namespace s4e_tmp;

struct Directory {
    std::string path;
    Directory() {
        require_filesystems();
        char name[] = "/var/tmp/s4e-command-missing-XXXXXX";
        require(::mkdtemp(name) != nullptr, "cannot create command decision fixture");
        path = name;
        std::printf("S4E owned inspection paths: %s, %s/missing, /dev/shm\n",
                    path.c_str(), path.c_str());
    }
    ~Directory() { ::rmdir(path.c_str()); }
};

void expect_command_failure(const std::string& candidate, int number) {
    bool caught = false;
    try {
        (void)pineforge::detail::make_command_scratch_backing(4096);
    } catch (const std::system_error& error) {
        const auto failure = pineforge::classify_run_failure(error);
        require(std::string(error.what()).find("command scratch create") == 0,
                "both command refusals must start with the established create prefix");
        require(error.code() == std::error_code(number, std::generic_category())
                    && failure.code == pineforge::RunFailureCode::scratch_io_failed
                    && !failure.args
                    && std::string(error.what()).find(candidate) != std::string::npos,
                "command backing must preserve its errno, candidate and resource failure code");
        caught = true;
    }
    require(caught, "command directory refusal unexpectedly succeeded");
    Host host;
    run(host);
    require(host.last_run_status() != 0 && host.callbacks == 0
                && host.native_state().kind != pineforge::NativeLifecycleKind::Completed
                && std::string(pineforge::run_failure_code_of(host)) == "scratch_io_failed"
                && std::string(pineforge::run_failure_args_of(host)) == "{}"
                && host.last_error().find("command scratch create") != std::string::npos
                && host.last_error().find(candidate) != std::string::npos
                && host.last_error().find(
                    std::error_code(number, std::generic_category()).message()) != std::string::npos,
            "command refusal must fail the native run at begin, before strategy callbacks");
    require(pineforge::run_failure_code_class(pineforge::RunFailureCode::scratch_io_failed)
                == pineforge::RunFailureClass::resource
                && !pineforge::run_failure_code_retryable(
                    pineforge::RunFailureCode::scratch_io_failed),
            "command refusal is a non-retryable resource failure");
}

void row() {
    PlainEnvironment environment;
    Directory directory;
    {
        const auto missing = directory.path + "/missing";
        Environment temporary("TMPDIR", missing.c_str());
        expect_command_failure(missing, ENOENT);
    }
    // D2 = keep: an explicit memory-backed command directory is refused
    // even when TMPDIR and /var/tmp offer healthy disk-backed candidates.
    Environment temporary("TMPDIR", directory.path.c_str());
    Environment command("PINEFORGE_COMMAND_SCRATCH_DIR", "/dev/shm");
    expect_command_failure("/dev/shm", ENODEV);
}
} // namespace

int main() {
    return s4e_tmp::test_main("test_command_scratch_missing_tmpdir", row);
}
