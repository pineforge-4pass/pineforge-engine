// Run the entrypoint test with CMake's configured Python.
#include <cerrno>
#include <cstring>
#include <iostream>
#include <unistd.h>

int main() {
    execl(PF_ISSUE_958_PYTHON, PF_ISSUE_958_PYTHON, "-B", PF_ISSUE_958_TEST,
          static_cast<char*>(nullptr));
    const int error = errno;
    std::cerr << "NOT COMPARED: expected to execute " << PF_ISSUE_958_PYTHON
              << "; got " << std::strerror(error) << '\n';
    return 1;
}
