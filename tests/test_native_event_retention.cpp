// R5 lane V19-B: what a run keeps of its event record (NativeRunSpec::
// event_retention) and how a host that polls it says what it has read
// (NativeStrategyHost::native_acknowledge_events). Source-free, so it also
// runs in the kernel-only profile.
//
//   1. the spec: Window is the default and folds nothing into the spec
//      digest -- the digest 6211dc94 computed for the same spec -- while Full
//      and Commands each fold; a word outside the enumeration is refused as
//      UnknownEventRetention at EventRetention.
#include <pineforge/native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <string>

using namespace pineforge;

namespace {
int checks = 0;
int failures = 0;
#define CHECK(x)                                                        \
    do {                                                                \
        ++checks;                                                       \
        if (!(x)) {                                                     \
            ++failures;                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);    \
        }                                                               \
    } while (0)

NativeRunSpec configuration(const char* key, uint64_t run = 1) {
    NativeRunSpec s;
    s.identity = {key, run};
    s.input_tf = "1";
    s.script_tf = "1";
    s.ticker = "N";
    s.tickerid = "TEST:N";
    s.type = "crypto";
    s.currency = "USD";
    s.basecurrency = "USD";
    s.description = "V19-B retention";
    s.volumetype = "base";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 10000;
    s.point_value = 1;
    s.account_fx = 1;
    s.price_tick = .01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0;
    return s;
}

// native_run_spec_digest(configuration("event-retention")) on 6211dc94,
// before the field existed (V19-B-scratch/failbefore/retention_spec_digest_base.cpp).
constexpr std::uint64_t kDigestBeforeTheField = 12094809940376900009ULL;

struct IdleHost final : NativeStrategyHost {
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}
};

// ── 1. the spec ───────────────────────────────────────────────────────────
void the_spec_names_a_retention() {
    NativeRunSpec window = configuration("event-retention");
    CHECK(window.event_retention == NativeEventRetention::Window);
    CHECK(native_run_spec_digest(window) == kDigestBeforeTheField);
    NativeRunSpec full = window;
    full.event_retention = NativeEventRetention::Full;
    NativeRunSpec commands = window;
    commands.event_retention = NativeEventRetention::Commands;
    CHECK(validate_native_run_spec(full).ok());
    CHECK(validate_native_run_spec(commands).ok());
    CHECK(native_run_spec_digest(full) != kDigestBeforeTheField);
    CHECK(native_run_spec_digest(commands) != kDigestBeforeTheField);
    CHECK(native_run_spec_digest(full) != native_run_spec_digest(commands));

    NativeRunSpec unknown = window;
    unknown.event_retention = static_cast<NativeEventRetention>(3);
    const auto refused = validate_native_run_spec(unknown);
    CHECK(refused.error == NativeRunSpecError::UnknownEventRetention);
    CHECK(refused.field == NativeRunSpecField::EventRetention);
    IdleHost host;
    const auto setup = host.configure_native(unknown);
    CHECK(setup.status == NativeSetupStatus::Failed);
    CHECK(setup.validation.error == NativeRunSpecError::UnknownEventRetention);
    CHECK(setup.validation.field == NativeRunSpecField::EventRetention);
    IdleHost fresh;
    CHECK(fresh.configure_native(full).status == NativeSetupStatus::Applied);
}
}  // namespace

int main() {
    the_spec_names_a_retention();
    if (failures != 0) {
        std::printf("test_native_event_retention: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("test_native_event_retention: ok (%d checks)\n", checks);
    return 0;
}
