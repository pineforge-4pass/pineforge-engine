#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

int main() {
    // Run the real Python grader test with the interpreter CMake selected.
    execl(PF_TEST_PYTHON, PF_TEST_PYTHON, "-I", "-B", PF_TEST_SCRIPT,
          static_cast<char*>(nullptr));
    std::fprintf(stderr, "NOT COMPARED #836: grader test launch; expected=started, actual=%s\n",
                 std::strerror(errno));
    return 2;
}
