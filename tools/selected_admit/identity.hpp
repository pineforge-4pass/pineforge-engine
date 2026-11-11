#pragma once

#include "result.hpp"
#include <pineforge/version.h>

namespace pineforge::selected_admit {
std::string_view compiled_git_sha() noexcept;

inline std::string describe() {
#if PF_SELECTED_ADMIT_HAVE_STAGE_SOURCE
    constexpr char stages[] = "[\"source\",\"selected\"]";
#else
    constexpr char stages[] = "[\"selected\"]";
#endif
    return manifest_projection(PF_SELECTED_ADMIT_HELPER_PATH, PINEFORGE_VERSION_FULL,
                               PINEFORGE_VERSION_STRING, compiled_git_sha(),
                               PF_SELECTED_ADMIT_DIRTY != 0, PF_SELECTED_ADMIT_SPEC_SHA256,
                               PF_SELECTED_ADMIT_CATALOG_SHA256, stages);
}
}  // namespace pineforge::selected_admit
