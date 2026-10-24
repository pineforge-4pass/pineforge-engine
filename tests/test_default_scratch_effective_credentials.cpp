// Coverage for round 5's AT_EACCESS fix: plain access() makes the child fail.
#include "scratch_candidate_message_fixture.hpp"
#include "scratch_process_fixture.hpp"
#include <pineforge/detail/scratch_file.hpp>

namespace {
void nonroot_row() {
    using namespace s4e_tmp;
    require(::geteuid() != 0 && ::getuid() == ::geteuid(),
            "non-root controls require matching real and effective uids");
    scratch_child("non-root effective-credentials refusal", [] {
        errno = 0;
        const int result = ::setreuid(65534, 0);
        const int observed_error = errno;
        require(result == -1 && observed_error == EPERM,
                "non-root setreuid(65534, 0) must be refused with EPERM");
        std::printf("non-root setreuid(65534, 0): PASS (errno %d = EPERM; "
                    "different-credentials cell is root-only)\n", observed_error);
    });

    PlainEnvironment environment;
    CandidateDirectory directory;
    require(::chmod(directory.path.c_str(), 0700) == 0,
            "cannot make non-root TMPDIR searchable by its owner");
    struct stat status{};
    require(::stat(directory.path.c_str(), &status) == 0
                && S_ISDIR(status.st_mode) && status.st_uid == ::geteuid()
                && (status.st_mode & 0777) == 0700,
            "non-root positive control must own its mode-0700 TMPDIR");
    require(::access(directory.path.c_str(), X_OK) == 0,
            "matching credentials must be able to search their owned TMPDIR");
    Environment temporary("TMPDIR", directory.path.c_str());
    std::string refusal;
    int error = 0;
    bool proven_memory = false;
    require(pineforge::detail::resolve_scratch_directory(
                "", refusal, error, proven_memory) == directory.path
                && error == 0 && refusal.empty() && !proven_memory,
            "matching non-root credentials must select their owned TMPDIR");
    {
        pineforge::detail::ScratchFile file;
        file.bound_cache();
        file.open("pineforge-nonroot-effective-credentials");
        require(file.opened(), "non-root credentials must permit a real scratch create");
        exercise_command_scratch(pineforge::detail::ScratchBackingKind::disk);
    }

    require(::chmod(directory.path.c_str(), 0000) == 0,
            "cannot make non-root TMPDIR unsearchable");
    require(::geteuid() != 0, "mode-0000 TMPDIR control must run without root DAC override");
    errno = 0;
    require(::access(directory.path.c_str(), X_OK) != 0 && errno == EACCES,
            "non-root mode-0000 TMPDIR must fail the search check with EACCES");
    std::uint64_t type = 0;
    require(pineforge::detail::scratch_directory_filesystem_type(directory.path.c_str(), type)
                == EACCES,
            "non-root mode-0000 TMPDIR probe must return EACCES");
    require(pineforge::detail::resolve_scratch_directory(
                "", refusal, error, proven_memory).empty()
                && error == EACCES && !proven_memory
                && refusal.find("TMPDIR '" + directory.path + "'") != std::string::npos,
            "non-root mode-0000 TMPDIR must be refused with EACCES without fallback");
    require(::chmod(directory.path.c_str(), 0700) == 0,
            "cannot restore non-root TMPDIR permissions");
}

void row() {
    using namespace s4e_tmp;
    PlainEnvironment environment;
    CandidateDirectory directory;
    require(::chmod(directory.path.c_str(), 0700) == 0,
            "cannot make effective-credentials fixture root-only");
    scratch_child("effective-credentials scratch", [&] {
        require(::setreuid(65534, 0) == 0, "cannot set different real and effective uids");
        require(::getuid() == 65534 && ::geteuid() == 0,
                "effective-credentials cell requires real uid 65534 and effective uid 0");
        Environment temporary("TMPDIR", directory.path.c_str());
        errno = 0;
        require(::access(directory.path.c_str(), X_OK) != 0 && errno == EACCES,
                "fixture must reject the real uid, so reverting AT_EACCESS is observable");
        std::string refusal;
        int error = 0;
        bool proven_memory = false;
        require(pineforge::detail::resolve_scratch_directory(
                    "", refusal, error, proven_memory) == directory.path
                    && error == 0 && refusal.empty() && !proven_memory,
                "root effective uid must select its root-only TMPDIR");
        pineforge::detail::ScratchFile file;
        file.bound_cache();
        file.open("pineforge-effective-credentials");
        require(file.opened(), "effective credentials must permit a real scratch create");
        exercise_command_scratch(pineforge::detail::ScratchBackingKind::disk);
    });
}
} // namespace

int main() {
    if (::geteuid() != 0) {
        std::puts("test_default_scratch_effective_credentials: running NON-ROOT controls");
        return s4e_tmp::test_main("test_default_scratch_effective_credentials", nonroot_row);
    }
    std::puts("test_default_scratch_effective_credentials: running ROOT credential cell");
    return s4e_tmp::test_main("test_default_scratch_effective_credentials", row);
}
