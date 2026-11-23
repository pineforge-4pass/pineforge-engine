// SPDX-License-Identifier: Apache-2.0
#include "../runner/report_spool.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace fs = std::filesystem;
using pineforge::live::ReportSpool;
constexpr char spool_prefix[] = ".pineforge-report-spool-";
bool refuse_unlink = false;
int refused_unlinks = 0;

struct TempDir {
    fs::path path;
    TempDir() {
        auto name = (fs::temp_directory_path() / "pineforge-spool-unlink-XXXXXX").string();
        if (!::mkdtemp(name.data())) throw std::runtime_error("cannot create test directory");
        path = name;
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

bool spool_name(const char* path) {
    const auto* slash = std::strrchr(path, '/');
    return std::strncmp(slash ? slash + 1 : path, spool_prefix, sizeof(spool_prefix) - 1) == 0;
}

std::string named_spools(const fs::path& directory) {
    std::string result;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (spool_name(entry.path().filename().c_str()))
            result += "\n" + entry.path().string();
    }
    return result;
}

} // namespace

extern "C" int __real_open(const char*, int, ...);
extern "C" int __wrap_open(const char* path, int flags, ...) {
#ifdef O_TMPFILE
    // Exercise mkstemp even on filesystems that support unnamed files.
    if ((flags & O_TMPFILE) == O_TMPFILE) {
        errno = EOPNOTSUPP;
        return -1;
    }
#endif
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        const auto mode = va_arg(args, int);
        va_end(args);
        return __real_open(path, flags, mode);
    }
    return __real_open(path, flags);
}

extern "C" int __real_unlink(const char*);
extern "C" int __wrap_unlink(const char* path) {
    if (refuse_unlink && spool_name(path)) {
        // Refuse only the first attempt so the cleanup can remove the file.
        refuse_unlink = false;
        ++refused_unlinks;
        errno = EACCES;
        return -1;
    }
    return __real_unlink(path);
}

int main() {
    try {
        TempDir directory;
        refuse_unlink = true;
        std::string refusal;
        try {
            ReportSpool spool(directory.path.string());
        } catch (const std::runtime_error& error) {
            refusal = error.what();
        }
        const auto left = named_spools(directory.path);
        if (refused_unlinks != 1 || refusal != "cumulative report spool cannot be created" || !left.empty())
            throw std::runtime_error("expected a create refusal and no named spools; refusal: " +
                                     refusal + "; named spools:" + left);

        ReportSpool spool(directory.path.string());
        const auto named = named_spools(directory.path);
        if (!named.empty()) throw std::runtime_error("successful spool has a name:" + named);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
