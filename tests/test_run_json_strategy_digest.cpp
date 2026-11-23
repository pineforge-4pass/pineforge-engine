#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

int main() {
    ::execl(PF_ISSUE_956_PYTHON, PF_ISSUE_956_PYTHON, "-I", "-B",
            PF_ISSUE_956_TEST, static_cast<char*>(nullptr));
    const int error = errno;
    std::fprintf(stderr, "NOT COMPARED Python launch: observed=%s, expected=running test\n",
                 std::strerror(error));
    return 1;
}
