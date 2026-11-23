#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

int main() {
    // The production CSV readers live in Python; run their test in this row.
    const std::string source = __FILE__;
    const auto slash = source.find_last_of('/');
    if (slash == std::string::npos) {
        std::fputs("NOT COMPARED: test source path; expected a directory, got a bare filename\n",
                   stderr);
        return 1;
    }
    const auto script = source.substr(0, slash + 1) + "test_issue_932_csv_timestamps.py";
    ::execlp("python3", "python3", "-B", script.c_str(), static_cast<char*>(nullptr));
    std::fprintf(stderr, "NOT COMPARED: expected python3 to run %s; got %s\n",
                 script.c_str(), std::strerror(errno));
    return 1;
}
