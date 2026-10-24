// Coverage for round 5's AT_EACCESS fix: plain access() makes the child fail.
#include "scratch_candidate_message_fixture.hpp"
#include "scratch_process_fixture.hpp"
#include <pineforge/detail/scratch_file.hpp>

namespace {
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
        std::puts("SKIP test_default_scratch_effective_credentials: requires effective uid 0 "
                  "for setreuid(65534, 0)");
        return 77;
    }
    return s4e_tmp::test_main("test_default_scratch_effective_credentials", row);
}
