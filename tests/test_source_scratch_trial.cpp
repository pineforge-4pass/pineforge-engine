// Fail-before base ceb399fb70: the first include is absent. The lab must
// record the first compiler diagnostic against that frozen closure.
#include <pineforge/detail/scratch_trial.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/resource.h>

namespace {
using namespace pineforge;
using namespace pineforge::detail;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// docs/design/scratch-facts.md's worked vector, written as its wire fields,
// without calling either encoder. It over-approximates this no-order host:
// S={ab}, F={""}, E={x}, H={}, J={q}. Every roster is complete.
const std::string certificate =
    "01"
    "01" "0100000000000000" "0200000000000000" "6162"
    "01" "0100000000000000" "0000000000000000"
    "01" "0100000000000000" "0100000000000000" "78"
    "01" "0000000000000000"
    "01" "0100000000000000" "0100000000000000" "71";

ScratchTrialConfig config(const char* classification) {
    ScratchTrialConfig result;
    result.order_shapes_receipt =
        "{\"scratch_facts\":{\"version\":1,\"classification\":\""
        + std::string(classification) + "\",\"certificate\":\"" + certificate + "\",\"reasons\":[]}}";
    result.live_bounds = LiveBounds{0, 0, 0, 0, 0};
    result.declared_input_defaults.emplace("Entry", "ab");
    return result;
}

const std::array<Bar, 3> bars{{
    {100, 100, 100, 100, 1, 1704067200000LL},
    {100, 100, 100, 100, 1, 1704067260000LL},
    {100, 100, 100, 100, 1, 1704067320000LL},
}};

class FilelessScope {
public:
    FilelessScope() {
        require(::getrlimit(RLIMIT_NOFILE, &saved_) == 0, "get descriptor limit");
        auto next = saved_;
        next.rlim_cur = 0;
        require(::setrlimit(RLIMIT_NOFILE, &next) == 0, "deny new files");
    }
    ~FilelessScope() { (void)::setrlimit(RLIMIT_NOFILE, &saved_); }
private:
    struct rlimit saved_{};
};

class SourceProbe final : public source::PineStrategyHost {
public:
    int calculations = 0;
    std::shared_ptr<ScratchSlab> observed;
    std::string effective_entry;

    SourceProbe() {
        source::PineStrategyConfig settings;
        configure_pine_strategy(settings);
        // No timezone resource needs reading while descriptors are disabled.
        set_syminfo_timezone("UTC+01:00");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }
    void on_source_bar(const Bar&) override {
        ++calculations;
        require(scratch_trial_binding().slab_bound, "source callback follows slab binding");
        const auto& slab = scratch_tables_for_run();
        require(static_cast<bool>(slab), "source sees run-owned slab");
        if (!observed) observed = slab;
        require(observed == slab, "source keeps one owner throughout the run");
        effective_entry = get_input_string("Entry", "ab");
        require(adapter_.fixture_auxiliary_disk_bytes() == 0, "source id tables stay off disk");
    }
};

void source_binders_accept_the_derived_plan_before_first_bar() {
    SourceProbe host;
    require(host.set_scratch_trial(config("in_subset")) == 0, "install source trial");
    {
        FilelessScope fileless;
        host.run(bars.data(), static_cast<int>(bars.size()));
    }
    require(host.last_error().empty() && host.last_run_status() == 0
            && host.calculations == static_cast<int>(bars.size()), "source runs every bar");
    require(host.native_state().kind == NativeLifecycleKind::Completed, "source run completed");
    const auto facts = scratch_facts_from_receipt(1, "in_subset", certificate);
    require(static_cast<bool>(facts), "worked vector decodes");
    const auto expected = derive_scratch_manifest(*facts.value);
    require(static_cast<bool>(expected) && host.observed, "derived source slab exists");
    const auto& actual = host.observed->manifest();
    const auto& oracle = *expected.value;
    require(actual.K_e == oracle.K_e && actual.B_e == oracle.B_e
            && actual.K_c == oracle.K_c && actual.B_c == oracle.B_c
            && actual.U == oracle.U && actual.B_U == oracle.B_U
            && actual.V == oracle.V && actual.B_V == oracle.B_V
            && actual.R == oracle.R, "source binder receives F's plan without policy substitution");
    require(host.effective_entry == "ab", "source reads its declared input default");
}

void source_out_is_returned_before_projecting_a_native_spec() {
    SourceProbe host;
    require(host.set_scratch_trial(config("out_of_subset")) == 0, "install source OUT");
    {
        FilelessScope fileless;
        host.run(bars.data(), static_cast<int>(bars.size()));
    }
    require(host.scratch_trial_binding().classification
                == ScratchTrialClassification::needs_file_capable_worker
            && host.scratch_trial_binding().reason == ScratchTrialReason::producer_out_of_subset,
            "source OUT retains the typed worker verdict");
    require(host.native_state().kind == NativeLifecycleKind::Unconfigured && host.calculations == 0
            && !host.observed && host.last_run_status() != 0, "source provider never configured or ran");
}

void rich_begin_inputs_are_checked_before_the_source_provider_copies_them() {
    SourceProbe host;
    require(host.set_scratch_trial(config("in_subset")) == 0, "install default certificate");
    InputsMap changed{{"Entry", "outside-the-certificate"}};
    SymInfo symbol;
    {
        FilelessScope fileless;
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", changed, symbol);
    }
    require(host.scratch_trial_binding().classification
                == ScratchTrialClassification::needs_file_capable_worker
            && host.scratch_trial_binding().reason == ScratchTrialReason::effective_inputs_changed
            && host.native_state().kind == NativeLifecycleKind::Unconfigured && host.calculations == 0,
            "rich begin cannot admit an override outside V's default proof");
    require(host.get_input_string("Entry", "ab") == "ab",
            "rejected rich inputs were never installed by the source provider");
}
} // namespace

int main() {
    try {
        source_binders_accept_the_derived_plan_before_first_bar();
        source_out_is_returned_before_projecting_a_native_spec();
        rich_begin_inputs_are_checked_before_the_source_provider_copies_them();
        std::puts("test_source_scratch_trial: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_source_scratch_trial: %s\n", error.what());
        return 1;
    }
}
