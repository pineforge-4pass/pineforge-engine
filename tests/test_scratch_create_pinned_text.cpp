// Pin the create diagnostics of the scratch files and the wrappers that open
// them (command, report, id-pair and successor-roots scratch) through real
// opens, with explicit paths. No TMPDIR changes or injected failures.
#include "scratch_process_fixture.hpp"
#include "../src/command_scratch_production.hpp"
#include "../src/report_scratch.hpp"
#include <pineforge/detail/id_pair_scratch_file.hpp>
#include <pineforge/native_order.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace pd = pineforge::detail;
namespace no = pineforge::native_order;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Directory {
    std::string path, missing, unwritable;

    Directory() {
        std::puts("Fixture parent: /var/tmp; owned mkdtemp template: "
                  "/var/tmp/s4e-create-pinned-XXXXXX");
        char name[] = "/var/tmp/s4e-create-pinned-XXXXXX";
        require(::mkdtemp(name) != nullptr, "cannot create pinned-text directory");
        path = name;
        missing = path + "/missing";
        unwritable = path + "/mode0500";
        std::printf("Owned inspection paths: %s, %s (absent), %s (mode 0500)\n",
                    path.c_str(), missing.c_str(), unwritable.c_str());
        std::fflush(stdout);
        std::uint64_t type = 0;
        require(pd::scratch_directory_filesystem_type(path.c_str(), type) == 0
                    && !pd::scratch_filesystem_is_memory(type),
                "pinned-text fixture requires disk-backed /var/tmp");
        require(::mkdir(unwritable.c_str(), 0700) == 0
                    && ::chmod(unwritable.c_str(), 0500) == 0,
                "cannot create owned mode-0500 directory");
    }

    ~Directory() {
        ::chmod(unwritable.c_str(), 0700);
        ::rmdir(unwritable.c_str());
        ::rmdir(path.c_str());
    }
};

// Change only the command store's explicit directory; leave TMPDIR untouched.
struct CommandDirectory {
    bool present;
    std::string previous;

    explicit CommandDirectory(const std::string& directory)
        : present(std::getenv("PINEFORGE_COMMAND_SCRATCH_DIR") != nullptr) {
        if (present) previous = std::getenv("PINEFORGE_COMMAND_SCRATCH_DIR");
        require(::setenv("PINEFORGE_COMMAND_SCRATCH_DIR", directory.c_str(), 1) == 0,
                "cannot set explicit command directory");
    }
    ~CommandDirectory() {
        if (present) ::setenv("PINEFORGE_COMMAND_SCRATCH_DIR", previous.c_str(), 1);
        else ::unsetenv("PINEFORGE_COMMAND_SCRATCH_DIR");
    }
};

template<class Error, class Operation>
void expect_exact(Operation operation, int number, const char* prefix,
                  pineforge::RunFailureCode code) {
    try {
        operation();
    } catch (const Error& error) {
        require(error.code() == std::error_code(number, std::generic_category()),
                std::string(prefix) + " must retain its exact errno and category");
        // These operation strings come from 61ab57dac1's five create paths,
        // not from the current wrappers. Compare the entire legacy what().
        const std::system_error expected(number, std::generic_category(), prefix);
        require(std::strcmp(error.what(), expected.what()) == 0,
                std::string("expected '") + expected.what() + "', got '" + error.what() + "'");
        const auto failure = pineforge::classify_run_failure(error);
        require(failure.code == code, std::string(prefix) + " changed its failure code");
        require(code == pineforge::RunFailureCode::report_sink_failed
                    ? (failure.args && *failure.args == "{\"reason\":\"scratch_io\"}")
                    : !failure.args,
                std::string(prefix) + " changed its failure arguments");
        return;
    }
    throw std::runtime_error(std::string(prefix) + " unexpectedly succeeded");
}

void missing_creates(const Directory& directory) {
    for (const bool bounded : {false, true}) {
        pd::ScratchFile file;
        if (bounded) file.bound_cache();
        file.set_directory(directory.missing);
        expect_exact<pd::ReportScratchIoError>(
            [&] { file.open("pineforge-pinned-text"); }, ENOENT, "scratch create",
            pineforge::RunFailureCode::report_sink_failed);
    }
    pd::IdPairScratchFile pairs;
    expect_exact<pd::IdPairStoreError>(
        [&] { pairs.open(directory.missing); }, ENOENT, "id table create",
        pineforge::RunFailureCode::scratch_io_failed);
    expect_exact<pd::ReportScratchIoError>(
        [&] { pineforge::report_sink::ScratchFile file(directory.missing); },
        ENOENT, "report scratch create", pineforge::RunFailureCode::report_sink_failed);
    CommandDirectory command(directory.missing);
    expect_exact<std::system_error>(
        [] { (void)pd::make_command_scratch_backing(4096); },
        ENOENT, "command scratch create", pineforge::RunFailureCode::scratch_io_failed);
}

void unwritable_create(const Directory& directory) {
    if (::geteuid() == 0) {
        require(::chmod(directory.path.c_str(), 0711) == 0,
                "cannot make pinned-text parent searchable by the dropped child");
        require(::chown(directory.unwritable.c_str(), 65534, 65534) == 0,
                "cannot give the dropped child ownership of its mode-0500 directory");
    }
    s4e_tmp::scratch_unprivileged("mode-0500 pinned create cells", [&] {
        require(::geteuid() != 0, "mode-0500 create cells must run without root DAC override");
        struct stat status{};
        require(::stat(directory.unwritable.c_str(), &status) == 0
                    && S_ISDIR(status.st_mode) && status.st_uid == ::geteuid()
                    && (status.st_mode & 0777) == 0500,
                "mode-0500 create cells must own their searchable, unwritable directory");
        // Search permission and the filesystem query succeed. The actual mkstemp
        // in ScratchFile::open must fail because this owned directory is unwritable.
        std::uint64_t type = 0;
        require(pd::scratch_directory_filesystem_type(directory.unwritable.c_str(), type) == 0
                    && !pd::scratch_filesystem_is_memory(type),
                "mode-0500 directory must pass the real filesystem query");
        for (const bool bounded : {false, true}) {
            pd::ScratchFile file;
            if (bounded) file.bound_cache();
            file.set_directory(directory.unwritable);
            expect_exact<pd::ReportScratchIoError>(
                [&] { file.open("pineforge-pinned-permission"); }, EACCES, "scratch create",
                pineforge::RunFailureCode::report_sink_failed);
        }
    });
}

no::Request request() {
    no::Request value{no::Transact{1.0}, "moving", ""};
    value.trigger = no::Limit{90.0};
    return value;
}

void successor_create(const Directory& directory) {
    // test_successor_roots_ram remains the unchanged pinned witness. Exercise
    // the real wrapper here too so m6 also makes this new CTest row fail.
    no::WorkingRequestCore core{{"pinned-text-roots", 1}};
    core.set_successor_roots_directory(directory.missing);
    std::uint64_t incarnation = 1, ordinal = 1;
    const no::CommandContext context{};
    const auto submitted = core.apply_submit(request(), context, incarnation, ordinal).result;
    require(submitted.status == no::SubmitStatus::Accepted && submitted.handle,
            "pinned-text successor submit must be accepted");
    auto tail = *submitted.handle;
    ++incarnation;
    ++ordinal;
    core.retire_history(core.last_ordinal());
    const auto replace = [&] {
        const auto installed = core.apply_replace(tail, request(), context, incarnation, ordinal);
        require(installed.result.status == no::ReplaceStatus::Replaced
                    && installed.result.successor,
                "pinned-text successor replacement must be accepted");
        tail = *installed.result.successor;
        ++incarnation;
        ++ordinal;
        core.retire_history(core.last_ordinal());
    };
    for (int i = 0; i < 64; ++i) replace();
    const auto before = core.last_ordinal();
    expect_exact<pd::AuxiliaryScratchIoError>(
        replace, ENOENT, "native successor roots create",
        pineforge::RunFailureCode::scratch_io_failed);
    require(core.last_ordinal() == before && core.find_live(tail),
            "failed successor create must precede logical mutation");
}
} // namespace

int main() {
    try {
        Directory directory;
        missing_creates(directory);
        unwritable_create(directory);
        successor_create(directory);
        std::puts("test_scratch_create_pinned_text: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_scratch_create_pinned_text: %s\n", error.what());
        return 1;
    }
}
