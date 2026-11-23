#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "NOT COMPARED #922: override argument count: got " << argc - 1
                  << "; expected 0 or 1\n";
        return 1;
    }
    const std::string source = __FILE__;
    const auto slash = source.find_last_of('/');
    if (slash == std::string::npos) {
        std::cerr << "NOT COMPARED #922: test source path: got " << source
                  << "; expected a path with a directory\n";
        return 1;
    }
    const auto script = source.substr(0, slash + 1) + "test_issue_922_strategy_override.py";
    if (::access(script.c_str(), R_OK) != 0) {
        std::cerr << "NOT COMPARED #922: test script: got " << std::strerror(errno)
                  << "; expected readable " << script << '\n';
        return 1;
    }
    ::execlp("python3", "python3", "-I", "-B", script.c_str(), argv[1], static_cast<char*>(nullptr));
    std::cerr << "NOT COMPARED #922: Python launch: got " << std::strerror(errno)
              << "; expected python3 to run " << script << '\n';
    return 1;
}
