#pragma once

#include "default_scratch_tmpfs_fixture.hpp"

#include <grp.h>
#include <sys/wait.h>

namespace s4e_tmp {

template<class Operation>
void scratch_child(const char* name, Operation operation) {
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t child = ::fork();
    require(child >= 0, std::string(name) + ": cannot fork");
    if (child == 0) {
        int result = 0;
        try {
            operation();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s: %s\n", name, error.what());
            result = 1;
        } catch (...) {
            std::fprintf(stderr, "%s: unknown child failure\n", name);
            result = 1;
        }
        std::fflush(stdout);
        std::fflush(stderr);
        ::_exit(result);
    }
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    require(waited == child, std::string(name) + ": cannot wait for child");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            std::string(name) + ": child failed (wait status " + std::to_string(status) + ")");
    std::printf("%s: child PASS\n", name);
}

template<class Operation>
void scratch_unprivileged(const char* name, Operation operation) {
    if (::geteuid() != 0) {
        operation();
        return;
    }
    scratch_child(name, [&] {
        require(::setgroups(0, nullptr) == 0, "cannot clear child supplementary groups");
        require(::setgid(65534) == 0, "cannot drop child gid to 65534");
        require(::setuid(65534) == 0, "cannot drop child uid to 65534");
        require(::getuid() == 65534 && ::geteuid() == 65534
                    && ::getgid() == 65534 && ::getegid() == 65534,
                "permission cells must execute with dropped credentials");
        operation();
    });
}

} // namespace s4e_tmp
