// C07: the cache reservation describes resident windows, never historical
// population. Runtime installation at two bar counts is checked by the Python
// host and real-library fixtures; this unit checks the shared layout algebra.
#include <pineforge/detail/scratch_defaults.hpp>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

int main() {
    using namespace pineforge::detail;
    const HpoScratchSpec spec{};
    const auto manifest = derive_hpo_scratch_manifest(spec);
    static_assert(sizeof(HpoScratchManifest) == 48);
    assert(manifest.struct_size == sizeof manifest);
    assert(manifest.version == 1);
    assert(manifest.root_bytes == 65536);
    assert(manifest.entry_bytes == 65536);
    assert(manifest.cohort_bytes == 65536);
    assert(manifest.transaction_bytes == 65536);
    assert(manifest.resident_bytes == 262144);

    // Independent, nondefault spec: a function returning the default manifest
    // for every input fails this row.
    HpoScratchSpec other;
    other.page_bytes = 8192;
    other.root_pages = 3;
    other.entry_pages = 5;
    other.cohort_pages = 7;
    other.transaction_pages = 11;
    const auto changed = derive_hpo_scratch_manifest(other);
    assert(changed.root_bytes == 24576);
    assert(changed.entry_bytes == 40960);
    assert(changed.cohort_bytes == 57344);
    assert(changed.transaction_bytes == 90112);
    assert(changed.resident_bytes == 212992);
    assert(std::memcmp(&manifest, &changed, sizeof manifest) != 0);

    other.page_bytes = 0;
    bool invalid = false;
    try { (void)derive_hpo_scratch_manifest(other); }
    catch (const std::invalid_argument&) { invalid = true; }
    assert(invalid);

    other = spec;
    other.root_pages = std::numeric_limits<std::uint64_t>::max();
    bool overflow = false;
    try { (void)derive_hpo_scratch_manifest(other); }
    catch (const std::overflow_error&) { overflow = true; }
    assert(overflow);

    // Each multiplication fits here, while their sum does not.
    other = spec;
    other.page_bytes = 1;
    other.root_pages = std::numeric_limits<std::uint64_t>::max();
    overflow = false;
    try { (void)derive_hpo_scratch_manifest(other); }
    catch (const std::overflow_error&) { overflow = true; }
    assert(overflow);
}
