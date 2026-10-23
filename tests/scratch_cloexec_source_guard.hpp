#pragma once

// D-02/D-03: inspect the actual creation expression, not the eventual fd flag.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <string>

namespace scratch_cloexec_test {
inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

inline void refuse_sensitive_path(const std::filesystem::path& path) {
    const auto text = path.generic_string();
    require(text.find("/.config/zsh/keys") == std::string::npos
                && text.find("/.config/pineforge") == std::string::npos,
            "source guard refuses a protected configuration path");
    for (const auto& part : path) {
        const auto name = part.string();
        require(name != ".ssh" && name != ".aws" && name != ".codex",
                "source guard refuses a protected directory");
        require(name.size() < 4 || name.compare(name.size() - 4, 4, ".env") != 0,
                "source guard refuses *.env");
    }
}

inline int check(const char* test_source, const char* relative_source,
                 const char* creation_expression) {
    try {
        const auto root = std::filesystem::path(test_source).parent_path().parent_path();
        const auto path = (root / relative_source).lexically_normal();
        // List the complete follow set before any filesystem inspection.
        std::fprintf(stderr, "Scratch CLOEXEC source guard will inspect: %s\n", path.c_str());
        std::fflush(stderr);
        refuse_sensitive_path(path);
        require(path.is_absolute(), "source guard requires an absolute test source path");
        // Refuse symlinks rather than following a link to a protected path.
        std::filesystem::path prefix;
        for (const auto& part : path) {
            prefix /= part;
            require(!std::filesystem::is_symlink(std::filesystem::symlink_status(prefix)),
                    "source guard refuses symlink components");
        }
        std::ifstream input(path);
        require(input.is_open(), "source guard opens its listed source");
        const std::string source{std::istreambuf_iterator<char>(input),
                                 std::istreambuf_iterator<char>()};
        require(!input.bad(), "source guard reads its listed source");
        require(!std::regex_search(source, std::regex(R"(\bmkstemp\s*\()")),
                "scratch creation must not expose an inheritable mkstemp fd");
        require(std::regex_search(source, std::regex(creation_expression)),
                "scratch creation must request O_CLOEXEC in mkostemp itself");
        require(source.find("F_SETFD") == std::string::npos,
                "scratch creation must not defer close-on-exec to fcntl");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Scratch CLOEXEC source guard: %s\n", error.what());
        return 1;
    }
}
} // namespace scratch_cloexec_test
