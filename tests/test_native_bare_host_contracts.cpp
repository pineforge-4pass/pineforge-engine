// R5 lane F3: contracts the kernel documents for EVERY host, which until this
// lane only the Pine adapter honoured (AUDIT3 §3.1 "What breaks G1", §7 F3).
// Each section below is the witness of one of them. Each was run against the
// lane's base (fd785928) before its change landed and failed there for the
// reason its header names; the lane report carries those runs.
//
// Source-free: this TU runs in the kernel-only profile.
#include <pineforge/native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

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

constexpr std::int64_t kT0 = 1704067200000LL;  // 2024-01-01 00:00 UTC
constexpr std::int64_t kMinute = 60000;

NativeRunSpec base_spec(const char* key, std::uint64_t run_number = 1) {
    NativeRunSpec s;
    s.identity = {key, run_number};
    s.input_tf = "1";
    s.script_tf = "1";
    s.ticker = "MOCK";
    s.tickerid = "TEST:MOCK";
    s.type = "crypto";
    s.currency = "USD";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 10000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    return s;
}

std::vector<Bar> flat_tape(int n, double price = 100.0) {
    std::vector<Bar> bars;
    for (int i = 0; i < n; ++i)
        bars.push_back(Bar{price, price, price, price, 1.0, kT0 + i * kMinute});
    return bars;
}

bool typed(const NativeSetupResult& r, NativeRunSpecError error, NativeRunSpecField field) {
    return r.status == NativeSetupStatus::Failed && r.validation.error == error
        && r.validation.field == field;
}

// ─── item 6: configure_native refused for its PHASE answers WrongPhase ─────
// NativeRunSpecError::WrongPhase is "the CALL was refused before any field was
// judged, because the host was not in the phase that call is legal in. Only a
// setup call answers it" (native_run_spec.hpp). configure_native is the setup
// call, yet it answered None for a Ready or Running host and CalendarFailure
// for a Failed one. Fails at the base on all three.
struct Idle : NativeStrategyHost {
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

struct ConfiguresWhileRunning : NativeStrategyHost {
    std::optional<NativeSetupResult> inner;
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (!inner) inner = configure_native(base_spec("f3-phase-running", 2));
    }
};

void configure_phase_refusal_is_wrong_phase() {
    {
        // Ready: the second call is refused, the host latches Contract (the
        // documented rule is unchanged), and the answer names the phase.
        Idle host;
        CHECK(host.configure_native(base_spec("f3-phase-ready")).status
              == NativeSetupStatus::Applied);
        const auto second = host.configure_native(base_spec("f3-phase-ready"));
        CHECK(typed(second, NativeRunSpecError::WrongPhase, NativeRunSpecField::None));
        const auto state = host.native_state();
        CHECK(state.kind == NativeLifecycleKind::Failed);
        CHECK(state.failure.code == NativeFailureCode::Contract);
    }
    {
        // Failed: a validation refusal fails the host; any later configure is
        // refused for the phase, not for a calendar.
        Idle host;
        NativeRunSpec bad = base_spec("f3-phase-failed");
        bad.initial_capital = 0.0;
        const auto first = host.configure_native(bad);
        CHECK(typed(first, NativeRunSpecError::NotFinitePositive,
                    NativeRunSpecField::InitialCapital));
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        const auto again = host.configure_native(base_spec("f3-phase-failed"));
        CHECK(typed(again, NativeRunSpecError::WrongPhase, NativeRunSpecField::None));
        CHECK(host.native_state().failure.code == NativeFailureCode::InvalidSpecification);
    }
    {
        // Running: a configure from inside a callback is refused for the
        // phase and latches Contract, as a Running refusal always did.
        ConfiguresWhileRunning host;
        CHECK(host.configure_native(base_spec("f3-phase-running")).status
              == NativeSetupStatus::Applied);
        const auto bars = flat_tape(3);
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.inner.has_value());
        if (host.inner) {
            CHECK(typed(*host.inner, NativeRunSpecError::WrongPhase, NativeRunSpecField::None));
        }
        const auto state = host.native_state();
        CHECK(state.kind == NativeLifecycleKind::Failed);
        CHECK(state.failure.code == NativeFailureCode::Contract);
    }
    {
        // The legal phases still apply: Unconfigured, and a Completed run
        // re-configured with a larger run number.
        Idle host;
        CHECK(host.configure_native(base_spec("f3-phase-legal")).status
              == NativeSetupStatus::Applied);
        const auto bars = flat_tape(2);
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
        const auto next = host.configure_native(base_spec("f3-phase-legal", 2));
        CHECK(next.status == NativeSetupStatus::Applied);
        CHECK(next.validation.error == NativeRunSpecError::None);
    }
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
    configure_phase_refusal_is_wrong_phase();
    std::printf("test_native_bare_host_contracts: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
