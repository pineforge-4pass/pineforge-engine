#include "scratch_capability_fixture.hpp"

#include <stdexcept>

namespace {
void expect(std::string_view line, bool known, bool setuid,
            bool dac_override, bool dac_read_search) {
    const auto parsed = s4e_tmp::parse_scratch_effective_capabilities(line);
    if (parsed.known != known || parsed.setuid != setuid
            || parsed.dac_override != dac_override
            || parsed.dac_read_search != dac_read_search)
        throw std::runtime_error("wrong capability parse for '" + std::string(line) + "'");
}

void expect_cells(const s4e_tmp::ScratchEffectiveCapabilities& capabilities,
                  std::string_view expected) {
    const auto actual = s4e_tmp::describe_cells(capabilities);
    if (actual != expected)
        throw std::runtime_error("wrong capability cell disclosure: " + actual);
}

void row() {
    expect("CapEff:\t0000000000000000", true, false, false, false);
    expect("CapEff:\t0000000000000080", true, true, false, false);
    expect("CapEff:\t0000000000000002", true, false, true, false);
    expect("CapEff:\t0000000000000004", true, false, false, true);
    expect("CapEff:\t000001ffffffffff", true, true, true, true);
    expect("malformed line", false, false, false, false);
    expect("CapPrm:\t0000000000000080", false, false, false, false);
    expect("CapEff:\t", false, false, false, false);
    expect("CapEff:\t0000000000000080junk", false, false, false, false);
    expect("CapEff:\t10000000000000000", false, false, false, false);
    expect("CapEff:\t0x80", false, false, false, false);
    expect("CapEff:\t-80", false, false, false, false);
    expect("CapEff:\t+80", false, false, false, false);
    expect("CapEff: 0000000000000080", true, true, false, false);
    expect("CapEff:\t0000000000000100", true, false, false, false);
    expect("CapEff:\t000001FFFFFFFFFF \t\r\n", true, true, true, true);

    const auto none = s4e_tmp::parse_scratch_effective_capabilities(
        "CapEff:\t0000000000000000");
    const auto unknown = s4e_tmp::parse_scratch_effective_capabilities("malformed line");
    if (none.may_setuid() || none.may_bypass_dac()
            || !unknown.may_setuid() || !unknown.may_bypass_dac())
        throw std::runtime_error("unknown capabilities must conservatively permit both overrides");

    expect_cells(none,
        "effective capabilities: none relevant; cells: setreuid EPERM, "
        "owned-0700 TMPDIR positive control, mode-0000 EACCES");
    expect_cells(s4e_tmp::parse_scratch_effective_capabilities("CapEff:\t0000000000000080"),
        "effective capabilities: CAP_SETUID(7); cells: "
        "setreuid EPERM omitted (CAP_SETUID may be present), "
        "child setreuid outcome and matching-credentials control, "
        "owned-0700 TMPDIR positive control, mode-0000 EACCES");
    expect_cells(s4e_tmp::parse_scratch_effective_capabilities("CapEff:\t0000000000000006"),
        "effective capabilities: CAP_DAC_OVERRIDE(1) CAP_DAC_READ_SEARCH(2); cells: "
        "setreuid EPERM, owned-0700 TMPDIR positive control, "
        "mode-0000 EACCES omitted (DAC capabilities may be present)");
    expect_cells(unknown,
        "effective capabilities: unknown (/proc/self/status CapEff missing, unreadable or malformed; "
        "capabilities may be present); cells: "
        "setreuid EPERM omitted (CAP_SETUID may be present), "
        "child setreuid outcome and matching-credentials control, "
        "owned-0700 TMPDIR positive control, "
        "mode-0000 EACCES omitted (DAC capabilities may be present)");
}
} // namespace

int main() {
    try {
        row();
        std::puts("test_scratch_capability_parse: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_scratch_capability_parse: %s\n", error.what());
        return 1;
    }
}
