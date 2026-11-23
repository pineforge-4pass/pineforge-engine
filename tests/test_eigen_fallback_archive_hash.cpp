#include <cerrno>
#include <cstring>
#include <iostream>
#include <unistd.h>

int main() {
    execl(PINEFORGE_ISSUE959_PYTHON, PINEFORGE_ISSUE959_PYTHON, "-I", "-B",
          PINEFORGE_ISSUE959_SCRIPT, PINEFORGE_ISSUE959_CMAKE,
          PINEFORGE_ISSUE959_CMAKELISTS, static_cast<char*>(nullptr));
    const int error = errno;
    std::cerr << "NOT COMPARED: could not start the Eigen fallback check; expected "
              << PINEFORGE_ISSUE959_PYTHON << ", observed " << std::strerror(error) << '\n';
    return 1;
}
