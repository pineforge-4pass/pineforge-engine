// Exercise the production identity header without a test-key definition.
#ifdef PINEFORGE_TEST_SOURCE_KEY_MASK
#error "the unverified-key control must use the shipped configuration"
#endif
#include <pineforge/source/live_source_identity.hpp>

#include <cstdio>
#include <exception>
#include <string>

namespace {
using pineforge::source::detail::LiveSourceIdentity;
int failures = 0;
#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
    ++failures; \
} } while (false)

void unclassified(const std::string& source, const std::string& from) {
    // Independent oracle: this suffix is neither a source id, pair/family,
    // close token nor a fixed internal spelling. No identity can be captured.
    // The production fallback must therefore impose no spelling constraint,
    // even when the next hash-only access has different text or placement ids.
    const auto key = source + "/future-replacement/" + from;
    const auto other = from + "/another-replacement/" + source;
    try {
        const auto identity = LiveSourceIdentity::capture(key, source, from);
        CHECK(identity.matches(key, source, from));
        CHECK(identity.matches(other, from, source));
        CHECK(identity.matches({}, {}, {}));
    } catch (const std::exception& error) {
        std::fprintf(stderr,
            "expected an unclassified live-source spelling to retain hash-only behavior; got: %s\n",
            error.what());
        ++failures;
    }

    // A represented raw id still requires exact bytes, including the suffix:
    // fallback must not disable identity checking for classified keys.
    const auto represented = LiveSourceIdentity::capture(source, source, from);
    CHECK(represented.matches(source, source, from));
    CHECK(!represented.matches(key, source, from));
}
} // namespace

int main() {
    unclassified("opening", "parent");
    unclassified(std::string(4096, 'q'), "");
    return failures == 0 ? 0 : 1;
}
