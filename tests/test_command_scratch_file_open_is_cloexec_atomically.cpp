// D-03. On base, CommandScratchFile creates an inheritable fd with mkstemp.
#include "scratch_cloexec_source_guard.hpp"

int main() {
    return scratch_cloexec_test::check(__FILE__,
        "src/command_scratch_production.cpp",
        R"(::mkostemp\s*\(\s*path\.data\s*\(\s*\)\s*,\s*O_CLOEXEC\s*\))");
}
