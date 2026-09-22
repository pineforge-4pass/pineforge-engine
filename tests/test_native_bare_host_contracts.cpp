// R5 lane F3: contracts the kernel documents for EVERY host, which until this
// lane only the Pine adapter honoured (AUDIT3 §3.1 "What breaks G1", §7 F3).
// Each section below is the witness of one of them. Each was run against the
// lane's base (fd785928) before its change landed and failed there for the
// reason its header names; the lane report carries those runs.
//
// Source-free: this TU runs in the kernel-only profile.
#include <pineforge/native_host.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace pineforge;

namespace {

int passed = 0;
int failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            ++passed;                                                            \
        } else {                                                                 \
            ++failed;                                                            \
            std::fprintf(stderr, "CHECK FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

std::vector<std::string> split_paths(const char* joined) {
    std::vector<std::string> out;
    std::string current;
    for (const char* c = joined; *c; ++c) {
        if (*c == '|') {
            if (!current.empty()) out.push_back(current);
            current.clear();
        } else {
            current.push_back(*c);
        }
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// The code of a C/C++ file with its comments blanked out. String and character
// literals are kept whole, so a literal that looks like a comment stays code.
std::string code_only(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    enum class Mode { Code, Line, Block, String, Char } mode = Mode::Code;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const char next = i + 1 < text.size() ? text[i + 1] : '\0';
        switch (mode) {
        case Mode::Code:
            if (c == '/' && next == '/') { mode = Mode::Line; ++i; continue; }
            if (c == '/' && next == '*') { mode = Mode::Block; ++i; continue; }
            if (c == '"') mode = Mode::String;
            if (c == '\'') mode = Mode::Char;
            out.push_back(c);
            break;
        case Mode::Line:
            if (c == '\n') { mode = Mode::Code; out.push_back(c); }
            break;
        case Mode::Block:
            if (c == '*' && next == '/') { mode = Mode::Code; ++i; }
            break;
        case Mode::String:
        case Mode::Char:
            out.push_back(c);
            if (c == '\\' && next != '\0') { out.push_back(next); ++i; continue; }
            if ((mode == Mode::String && c == '"') || (mode == Mode::Char && c == '\''))
                mode = Mode::Code;
            break;
        }
    }
    return out;
}

// ─── item 8: the adapter's `__close__` id prefix is not kernel code ─────────
// A strategy.close order id is the source adapter's own spelling. The kernel
// carried a copy of that prefix (`internal::kClosePrefix`) with no reader left
// anywhere in the tree: dead, Pine-shaped code in every object that includes
// the kernel's internal header. Fails at the base: engine_internal.hpp still
// defines it.
void close_prefix_is_not_kernel_code() {
    const auto files = split_paths(PINEFORGE_F3_KERNEL_FILES);
    CHECK(files.size() > 40);
    int readable = 0;
    for (const auto& path : files) {
        const std::string text = read_file(path);
        if (!text.empty()) ++readable;
        const std::string code = code_only(text);
        const bool carries = code.find("\"__close__\"") != std::string::npos;
        if (carries) std::fprintf(stderr, "  __close__ literal in kernel code: %s\n", path.c_str());
        CHECK(!carries);
    }
    CHECK(readable == static_cast<int>(files.size()));
}

}  // namespace

int main() {
    close_prefix_is_not_kernel_code();
    std::printf("test_native_bare_host_contracts: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
