#pragma once

#include "default_scratch_tmpfs_fixture.hpp"
#include <system_error>

namespace s4e_tmp {

struct CandidateDirectory {
    std::string path, missing;
    CandidateDirectory() {
        require_filesystems();
        char name[] = "/var/tmp/s4e-candidate-message-XXXXXX";
        require(::mkdtemp(name) != nullptr, "cannot create candidate-message fixture");
        path = name;
        missing = path + "/missing";
        std::printf("S4E owned inspection paths: %s, %s (deliberately absent)\n",
                    path.c_str(), missing.c_str());
    }
    ~CandidateDirectory() { ::rmdir(path.c_str()); }
};

template<class Error, class Operation>
void expect_candidate_failure(Operation operation, const char* prefix,
                              const std::string& candidate,
                              pineforge::RunFailureCode code) {
    try {
        operation();
    } catch (const Error& error) {
        const auto failure = pineforge::classify_run_failure(error);
        require(error.code() == std::error_code(ENOENT, std::generic_category())
                    && failure.code == code,
                "create translator must preserve errno and typed failure code");
        require(failure.args && *failure.args ==
                    (code == pineforge::RunFailureCode::report_sink_failed
                        ? "{\"reason\":\"scratch_io\"}" : "{}"),
                "create translator must preserve its established failure arguments");
        const std::string text = error.what();
        require(text.find(prefix) == 0
                    && text.find("TMPDIR '" + candidate + "': statfs failed") != std::string::npos
                    && text.find("set TMPDIR") == std::string::npos,
                "create translator must retain its prefix and the failed query candidate");
        return;
    }
    throw std::runtime_error("missing TMPDIR unexpectedly succeeded");
}

} // namespace s4e_tmp
