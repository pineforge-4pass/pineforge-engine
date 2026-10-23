#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace p2cx_fixture {

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Keep every fixture artifact under this checkout's scratch-s4e. Leave the
// unique directory intact for the lab's red/fixed/mutation result inspection.
struct Directory {
    std::string path;

    explicit Directory(const char* finding) {
        const auto root = std::filesystem::path(__FILE__).parent_path().parent_path()
            / "scratch-s4e";
        std::filesystem::create_directories(root);
        path = (std::filesystem::canonical(root)
            / (std::string("p2cx-") + finding + "-XXXXXX")).string();
        require(::mkdtemp(path.data()) != nullptr, "create private fixture directory");
    }

    std::string child(const char* name) const { return path + '/' + name; }
};

inline std::vector<std::string> entries(const std::string& path, const char* stage) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(path))
        names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    std::fprintf(stderr, "%s %s:", stage, path.c_str());
    for (const auto& name : names) std::fprintf(stderr, " %s", name.c_str());
    std::fprintf(stderr, "\n");
    return names;
}

struct Tmpdir {
    bool existed = std::getenv("TMPDIR") != nullptr;
    std::string previous = existed ? std::getenv("TMPDIR") : "";

    explicit Tmpdir(const std::string& path) {
        require(::setenv("TMPDIR", path.c_str(), 1) == 0, "set fixture TMPDIR");
    }
    ~Tmpdir() {
        if (existed) ::setenv("TMPDIR", previous.c_str(), 1);
        else ::unsetenv("TMPDIR");
    }
};

} // namespace p2cx_fixture
