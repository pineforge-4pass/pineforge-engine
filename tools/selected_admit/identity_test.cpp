#include "identity.hpp"

namespace pineforge::selected_admit {
std::string_view compiled_git_sha() noexcept { return PF_SELECTED_ADMIT_TEST_GIT_SHA; }
}  // namespace pineforge::selected_admit
