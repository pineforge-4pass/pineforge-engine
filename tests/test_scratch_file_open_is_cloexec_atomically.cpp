// D-02. On base, ScratchFile::open calls mkstemp before F_SETFD.
#include "scratch_cloexec_source_guard.hpp"

int main() {
    return scratch_cloexec_test::check(__FILE__,
        "include/pineforge/detail/scratch_file.hpp",
        R"(::mkostemp\s*\(\s*name\.data\s*\(\s*\)\s*,\s*O_CLOEXEC\s*\))");
}
