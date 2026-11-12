#include "identity.hpp"

namespace pineforge::selected_admit {
std::string_view compiled_git_sha() noexcept { return PINEFORGE_GIT_SHA; }
}  // namespace pineforge::selected_admit
