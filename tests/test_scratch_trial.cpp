// Fail-before base: ceb399fb70. scratch_trial.hpp does not exist there.
// The supervisor must compile this complete TU against that header closure
// on the lab and record the first diagnostic. Nothing was compiled locally.
#include <pineforge/detail/scratch_trial.hpp>
#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/native_host.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
using namespace pineforge::detail;
using Rosters = std::array<std::vector<std::string>, 5>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// This fixture produces the documented wire, without the production encoder
// or canonicalizer. Its order comes from std::set, not ScratchByteLess.
std::string certificate(const Rosters& rosters, int incomplete = -1) {
    std::string bytes(1, '\1');
    const auto word = [&](std::uint64_t value) {
        for (unsigned byte = 0; byte != 8; ++byte) {
            bytes.push_back(static_cast<char>(value & 255));
            value >>= 8;
        }
    };
    for (std::size_t index = 0; index != rosters.size(); ++index) {
        bytes.push_back(static_cast<int>(index) == incomplete ? '\0' : '\1');
        const std::set<std::string> unique(rosters[index].begin(), rosters[index].end());
        word(unique.size());
        for (const auto& value : unique) {
            word(value.size());
            bytes.append(value);
        }
    }
    const char hex[] = "0123456789abcdef";
    std::string result;
    for (const unsigned char byte : bytes) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}

// Concrete-domain oracle: enumerate identities, rather than copying F's
// capacity arithmetic. NUL and the joined-key separator are ordinary bytes.
ScratchManifest enumerate(const Rosters& rosters) {
    ScratchManifest expected;
    std::set<std::tuple<bool, std::string, std::string>> erased;
    for (const auto& source : rosters[0]) {
        erased.emplace(false, source, "");
        for (const auto& from : rosters[1]) erased.emplace(true, source, from);
    }
    for (const auto& key : erased) {
        ++expected.K_e;
        expected.B_e += std::get<1>(key).size() + std::get<2>(key).size();
    }
    std::set<std::string> consumed;
    for (const auto& exit : rosters[2])
        for (const auto& from : rosters[1])
            consumed.insert(exit + '\x1f' + from);
    for (const auto& key : consumed) {
        ++expected.K_c;
        expected.B_c += key.size();
    }
    const std::set<std::string> cohorts(rosters[3].begin(), rosters[3].end());
    for (const auto& key : cohorts) {
        ++expected.U;
        expected.B_U += key.size();
    }
    const std::set<std::string> eligible(rosters[4].begin(), rosters[4].end());
    std::vector<std::string> longest(eligible.begin(), eligible.end());
    std::stable_sort(longest.begin(), longest.end(), [](const auto& left, const auto& right) {
        return left.size() > right.size();
    });
    for (const auto& key : longest) {
        if (expected.V == 27) break; // The specified rank population limit.
        ++expected.V;
        expected.B_V += key.size();
    }
    return expected;
}

void same_manifest(const ScratchManifest& actual, const ScratchManifest& expected) {
    require(actual.K_q == expected.K_q && actual.B_q == expected.B_q, "q stays absent");
    require(actual.K_e == expected.K_e && actual.B_e == expected.B_e, "erased domain");
    require(actual.K_c == expected.K_c && actual.B_c == expected.B_c, "joined domain");
    require(actual.U == expected.U && actual.B_U == expected.B_U, "cohort domain");
    require(actual.V == expected.V && actual.B_V == expected.B_V, "eligible domain");
    require(actual.R == expected.R, "live proof never becomes legacy historical R");
}

std::string document(std::string_view hex, std::string_view label = "in_subset",
                     unsigned version = 1) {
    return "{\"api_version\":1,\"shapes\":[{\"unused\":[null,true,false,-2.5e+3]}],"
           "\"scratch_facts\":{\"version\":" + std::to_string(version)
        + ",\"classification\":\"" + std::string(label)
        + "\",\"certificate\":\"" + std::string(hex) + "\",\"reasons\":[]}}";
}

Rosters domain(const std::string& id) {
    return {{{id, id}, {"", id}, {"exit", std::string("a\x1f", 2)},
             {"", id, std::string("a\0b", 3)}, {id, "rank"}}};
}

ScratchTrialConfig trial(const Rosters& rosters) {
    ScratchTrialConfig config;
    config.order_shapes_receipt = document(certificate(rosters));
    // This witness never submits or replaces a request, creates a cohort, or
    // keeps a handle. These are proved zero populations, not missing bounds.
    config.live_bounds = LiveBounds{0, 0, 0, 0, 0};
    return config;
}

NativeRunSpec spec() {
    NativeRunSpec value;
    value.identity = {"scratch-trial-witness", 1};
    value.input_tf = value.script_tf = "1";
    value.ticker = "MOCK";
    value.tickerid = "TEST:MOCK";
    value.type = "crypto";
    value.currency = "USD";
    // A fixed offset has no TZif resource to read under FilelessScope.
    value.timezone = "UTC+01:00";
    value.session = "24x7";
    value.initial_capital = 10000.0;
    value.point_value = value.account_fx = 1.0;
    value.price_tick = 0.01;
    value.fee_kind = NativeFeeKind::CashPerExecution;
    value.fee_value = 0.0;
    value.report_policy = NativeReportPolicy::KernelRecorded;
    return value;
}

const std::array<Bar, 3> bars{{
    {100, 100, 100, 100, 1, 1704067200000LL},
    {100, 100, 100, 100, 1, 1704067260000LL},
    {100, 100, 100, 100, 1, 1704067320000LL},
}};

// The kernel and source file backends use real OS descriptors. With the soft
// limit at zero, a mistakenly entered file backend cannot create even an
// immediately unlinked scratch file. There is no mock storage success path.
class FilelessScope {
public:
    FilelessScope() {
        require(::getrlimit(RLIMIT_NOFILE, &saved_) == 0, "read descriptor limit");
        auto limited = saved_;
        limited.rlim_cur = 0;
        require(::setrlimit(RLIMIT_NOFILE, &limited) == 0, "enter fileless witness");
    }
    ~FilelessScope() { (void)::setrlimit(RLIMIT_NOFILE, &saved_); }
    FilelessScope(const FilelessScope&) = delete;
    FilelessScope& operator=(const FilelessScope&) = delete;
private:
    struct rlimit saved_{};
};

class Probe final : public NativeStrategyHost {
public:
    int prepares = 0, begins = 0, calculations = 0;
    std::shared_ptr<ScratchSlab> observed;
    ScratchFile row;
    bool write_row = true;
    std::uint8_t marker = 17;

    void prepare_native_begin(const NativeBeginArgs&) override {
        ++prepares;
        observed = scratch_tables_for_run();
        require(observed && scratch_trial_binding().slab_bound, "slab bound before provider");
        auto next = spec();
        next.identity.run_number = static_cast<std::uint64_t>(prepares);
        require(configure_native(next).status == NativeSetupStatus::Applied, "spec applied");
        row.set_directory("/dev/null/no-scratch-trial-fallback");
        row.bind_scratch(observed, ScratchResource::erased_data);
    }
    void on_native_run_begin() override {
        ++begins;
        require(scratch_tables_for_run() == observed, "same owner at semantic begin");
    }
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++calculations;
        require(scratch_tables_for_run() == observed, "owner never changes during a run");
        // Real backend copies, within an enumerated nonempty domain. For the
        // empty-domain witness there is intentionally no row to write.
        if (write_row) {
            row.open("scratch-trial-must-not-open");
            row.write(0, &marker, sizeof marker);
            std::uint8_t actual = 0;
            row.read(0, &actual, sizeof actual);
            require(actual == marker, "first bar actually uses the bound slab");
        }
        require(set_scratch_trial(ScratchTrialConfig{}) != 0, "active trial mutation rejected");
    }
};

void check_out(std::string receipt, ScratchTrialReason reason,
               ScratchFactsError facts_error = ScratchFactsError::none) {
    Probe host;
    auto config = trial(domain("L"));
    config.order_shapes_receipt = std::move(receipt);
    require(host.set_scratch_trial(config) == 0, "install receipt under test");
    {
        FilelessScope fileless;
        host.run(bars.data(), static_cast<int>(bars.size()));
    }
    const auto& binding = host.scratch_trial_binding();
    require(binding.classification == ScratchTrialClassification::needs_file_capable_worker,
            "OUT has the typed worker-routing classification");
    require(binding.reason == reason && binding.facts_error == facts_error, "specific OUT reason");
    require(!binding.plan && !binding.slab_bound && !host.observed, "OUT publishes no finite backend");
    require(host.prepares == 0 && host.begins == 0 && host.calculations == 0,
            "OUT stops before provider, begin and bar zero");
    require(host.native_state().kind == NativeLifecycleKind::Unconfigured, "run never started");
    require(!host.last_error().empty(), "an unrouted run is failed");
    require(run_failure_value_of(host).code != RunFailureCode::scratch_capacity_exceeded
            && run_failure_value_of(host).code != RunFailureCode::scratch_io_failed,
            "routing is neither a capacity refusal nor attempted file I/O");
}

void receipt_absent() {
    check_out("", ScratchTrialReason::receipt_absent);
    check_out("{\"api_version\":1,\"shapes\":[]}", ScratchTrialReason::receipt_absent);
}

void receipt_malformed() {
    const auto valid = document(certificate(domain("L")));
    check_out(valid.substr(0, valid.size() - 1), ScratchTrialReason::receipt_malformed);
    check_out(valid + "null", ScratchTrialReason::receipt_malformed);
    check_out("{\"scratch_facts\":null}", ScratchTrialReason::receipt_malformed);
    check_out("{\"scratch_facts\":{}}", ScratchTrialReason::receipt_malformed);
    check_out(document("gg"), ScratchTrialReason::certificate_invalid, ScratchFactsError::invalid_length);
    check_out(document("01"), ScratchTrialReason::certificate_invalid, ScratchFactsError::truncated);
    check_out(document(certificate(domain("L")), "IN_SUBSET"), ScratchTrialReason::certificate_invalid,
              ScratchFactsError::invalid_complete_flag);
    check_out(document("aA"), ScratchTrialReason::certificate_invalid, ScratchFactsError::invalid_length);
    // A default-valued/missing field must not silently authorize the run.
    for (const std::string field : {"version", "classification", "certificate", "reasons"}) {
        auto missing = valid;
        const auto begin = missing.find("\"" + field + "\":", missing.find("\"scratch_facts\""));
        auto end = missing.find(',', begin);
        if (field == "reasons") {
            require(begin > 0, "fixture field found");
            missing.erase(begin - 1, missing.find('}', begin) - begin + 1);
        } else {
            require(end != std::string::npos, "fixture field delimiter found");
            missing.erase(begin, end - begin + 1);
        }
        check_out(missing, ScratchTrialReason::receipt_malformed);
    }
    auto duplicate = valid;
    duplicate.insert(duplicate.find("\"version\":1"), "\"version\":1,");
    check_out(duplicate, ScratchTrialReason::receipt_malformed);
    auto wrong_type = valid;
    wrong_type.replace(wrong_type.find("\"version\":1"), std::string("\"version\":1").size(),
                       "\"version\":\"1\"");
    check_out(wrong_type, ScratchTrialReason::receipt_malformed);
    for (const std::string replacement : {"\"reasons\":null", "\"reasons\":[1]", "\"reasons\":[\"x\",]"}) {
        auto malformed = valid;
        malformed.replace(malformed.find("\"reasons\":[]"), std::string("\"reasons\":[]").size(), replacement);
        check_out(malformed, ScratchTrialReason::receipt_malformed);
    }
    auto duplicate_root = valid;
    duplicate_root.insert(1, "\"scratch_facts\":{},");
    check_out(duplicate_root, ScratchTrialReason::receipt_malformed);
    for (const std::string skipped : {"[1,]", "{\"x\":}", "{\"x\":true false}", "01",
                                     "\"\\uD800\"", "\"\\uDC00\"", "\"\\q\""}) {
        auto malformed = valid;
        malformed.insert(1, "\"unknown\":" + skipped + ",");
        check_out(malformed, ScratchTrialReason::receipt_malformed);
    }
    for (std::size_t end = 1; end < valid.size(); ++end)
        check_out(valid.substr(0, end), ScratchTrialReason::receipt_malformed);
}

void receipt_unknown_version() {
    check_out(document(certificate(domain("L")), "in_subset", 2),
              ScratchTrialReason::certificate_invalid, ScratchFactsError::unsupported_version);
    auto hex = certificate(domain("L"));
    hex.replace(0, 2, "02");
    check_out(document(hex), ScratchTrialReason::certificate_invalid, ScratchFactsError::unsupported_version);
}

void receipt_incomplete_roster() {
    const auto rosters = domain("L");
    for (int index = 0; index != 5; ++index)
        check_out(document(certificate(rosters, index)), ScratchTrialReason::certificate_invalid,
                  ScratchFactsError::incomplete_vocabulary);
}

void receipt_out_of_subset() {
    // Even a complete roster labelled OUT must stay OUT: V's reader alone
    // accepts that certificate, so H must preserve the explicit classification.
    check_out(document(certificate(domain("L")), "out_of_subset"),
              ScratchTrialReason::producer_out_of_subset);
    check_out(document(certificate(domain("L"), 2), "out_of_subset"),
              ScratchTrialReason::producer_out_of_subset);
}

void in_subset_binds_derived_plan_and_runs() {
    const auto rosters = domain("longer-entry");
    auto config = trial(rosters);
    Probe host;
    require(host.set_scratch_trial(config) == 0, "install finite trial");
    // An explicit legacy manifest must not override the certificate plan.
    pf_scratch_tables_v1 legacy{};
    legacy.struct_size = static_cast<std::uint32_t>(sizeof legacy);
    legacy.version = 1;
    legacy.backend = PF_SCRATCH_TABLE_BOUNDED_MEMORY;
    legacy.K_e = 1;
    require(host.set_scratch_tables_v1(&legacy) == 0, "install unrelated legacy policy");
    {
        FilelessScope fileless;
        host.run(bars.data(), static_cast<int>(bars.size()));
    }
    require(host.last_error().empty() && host.prepares == 1 && host.begins == 1
            && host.calculations == static_cast<int>(bars.size()), "first bar and full batch run");
    const auto& binding = host.scratch_trial_binding();
    require(binding.classification == ScratchTrialClassification::in_subset && binding.plan
            && binding.slab_bound && host.observed, "finite plan and owner are published together");
    const auto facts = scratch_facts_from_receipt(1, "in_subset", certificate(rosters));
    require(static_cast<bool>(facts), "fixture decodes through V/F");
    const auto derived = derive_scratch_manifest(*facts.value);
    require(static_cast<bool>(derived), "F derives fixture");
    same_manifest(host.observed->manifest(), *derived.value);
    same_manifest(host.observed->manifest(), enumerate(rosters));
    same_manifest(binding.plan->tables, enumerate(rosters));
}

void all_zero_certificate_is_not_p0() {
    Probe host;
    host.write_row = false;
    const Rosters empty{};
    require(host.set_scratch_trial(trial(empty)) == 0, "install complete empty domain");
    {
        FilelessScope fileless;
        host.run(bars.data(), static_cast<int>(bars.size()));
    }
    require(host.last_error().empty() && host.observed && host.calculations != 0, "empty-domain run");
    same_manifest(host.observed->manifest(), enumerate(empty));
}

void per_trial_binding() {
    Probe first, second;
    const auto a = domain("A"), b = domain("different-trial-id");
    auto first_config = trial(a), second_config = trial(b);
    require(first.set_scratch_trial(first_config) == 0 && second.set_scratch_trial(second_config) == 0,
            "install independent HPO trials");
    first_config.order_shapes_receipt.clear();
    second_config.order_shapes_receipt.clear(); // Hosts own their receipt bytes.
    first.marker = 31;
    second.marker = 73;
    {
        FilelessScope fileless;
        first.run(bars.data(), static_cast<int>(bars.size()));
        second.run(bars.data(), static_cast<int>(bars.size()));
    }
    require(first.last_error().empty() && second.last_error().empty(), "both trials execute");
    require(first.observed != second.observed, "trials have independent mutable slabs");
    same_manifest(first.observed->manifest(), enumerate(a));
    same_manifest(second.observed->manifest(), enumerate(b));
    std::uint8_t actual = 0;
    first.row.read(0, &actual, sizeof actual);
    require(actual == first.marker, "second trial cannot overwrite first trial bytes");
    const auto old_owner = first.observed;
    require(first.set_scratch_trial(trial(b)) == 0, "rebind a reused HPO handle");
    {
        FilelessScope fileless;
        first.run(bars.data(), 1);
    }
    require(first.last_error().empty() && first.observed != old_owner, "reused trial constructs a fresh slab");
    same_manifest(first.observed->manifest(), enumerate(b));
    require(first.set_scratch_trial(ScratchTrialConfig{}) == 0, "replace finite trial with absent receipt");
    const int preceding_prepares = first.prepares;
    {
        FilelessScope fileless;
        first.run(bars.data(), 1);
    }
    require(first.scratch_trial_binding().classification
                == ScratchTrialClassification::needs_file_capable_worker
            && !first.scratch_trial_binding().slab_bound
            && first.prepares == preceding_prepares, "OUT cannot reuse a preceding trial's backend");
}

void effective_inputs_and_missing_live_proofs_route_out() {
    const auto base = trial(domain("L"));
    for (const bool changed_input : {false, true}) {
        Probe host;
        auto config = base;
        if (changed_input) {
            config.declared_input_defaults.emplace("Entry", "L");
            host.set_input("Entry", "outside-certified-roster");
        } else {
            config.live_bounds.reset();
        }
        require(host.set_scratch_trial(config) == 0, "install unproved trial");
        {
            FilelessScope fileless;
            host.run(bars.data(), 1);
        }
        require(host.scratch_trial_binding().classification
                    == ScratchTrialClassification::needs_file_capable_worker
                && host.prepares == 0, "unproved effective trial never reaches provider");
        require(host.scratch_trial_binding().reason == (changed_input
                    ? ScratchTrialReason::effective_inputs_changed : ScratchTrialReason::live_proof_missing),
                "effective-trial reason is preserved");
    }
    Probe matching_default;
    auto config = base;
    config.declared_input_defaults.emplace("Entry", "L");
    matching_default.set_input("Entry", "L");
    require(matching_default.set_scratch_trial(config) == 0, "install default-input witness");
    {
        FilelessScope fileless;
        matching_default.run(bars.data(), 1);
    }
    require(matching_default.last_error().empty() && matching_default.calculations == 1,
            "an override equal to its declared default keeps the default certificate valid");
    auto unknown_input = base;
    const auto unknown = plan_scratch_trial(unknown_input, {{"Undeclared", "L"}}, false);
    require(unknown.classification == ScratchTrialClassification::needs_file_capable_worker
            && unknown.reason == ScratchTrialReason::effective_inputs_changed,
            "missing declaration metadata is not proof of an input default");
    const auto overridden = plan_scratch_trial(base, {}, true);
    require(overridden.classification == ScratchTrialClassification::needs_file_capable_worker
            && overridden.reason == ScratchTrialReason::effective_overrides_unproved,
            "opaque overrides never inherit a default live proof");
    auto incomplete = base;
    incomplete.live_bounds->M.reset();
    const auto unproved = plan_scratch_trial(incomplete, {}, false);
    require(unproved.classification == ScratchTrialClassification::needs_file_capable_worker
            && unproved.reason == ScratchTrialReason::live_proof_missing,
            "a partially present live proof is still missing");
}

void nonzero_root_plans_wait_for_the_root_binder() {
    Probe host;
    auto config = trial(domain("L"));
    config.live_bounds = LiveBounds{1, 0, 0, 0, 0};
    require(host.set_scratch_trial(config) == 0, "install nonzero live proof");
    {
        FilelessScope fileless;
        host.run(bars.data(), 1);
    }
    require(host.scratch_trial_binding().classification
                == ScratchTrialClassification::needs_file_capable_worker
            && host.scratch_trial_binding().reason == ScratchTrialReason::root_backend_unavailable
            && host.prepares == 0 && host.calculations == 0,
            "a finite live proof must never be bound as historical R=0");
    config.live_bounds = LiveBounds{std::numeric_limits<std::uint64_t>::max(), 1, 0, 0, 0};
    require(host.set_scratch_trial(config) == 0, "install overflowing live proof");
    {
        FilelessScope fileless;
        host.run(bars.data(), 1);
    }
    require(host.scratch_trial_binding().classification
                == ScratchTrialClassification::needs_file_capable_worker
            && host.scratch_trial_binding().reason == ScratchTrialReason::plan_invalid
            && host.scratch_trial_binding().facts_error == ScratchFactsError::arithmetic_overflow
            && !host.scratch_trial_binding().plan && host.prepares == 0,
            "F's arithmetic error never authorizes a wrapped or policy plan");
}

void json_escaping_and_additive_fields() {
    auto config = trial(domain("L"));
    auto& json = config.order_shapes_receipt;
    json.replace(json.find("\"scratch_facts\""), std::string("\"scratch_facts\"").size(),
                 "\"scratch\\u005ffacts\"");
    json.replace(json.find("\"in_subset\""), std::string("\"in_subset\"").size(),
                 "\"in\\u005fsubset\"");
    // Validate a surrogate pair and every JSON escape in a skipped field.
    json.insert(1, "\"unknown\":\"\\uD83D\\uDE00\\\"\\\\\\/\\b\\f\\n\\r\\t\",");
    const auto planned = plan_scratch_trial(config, {}, false);
    require(planned.classification == ScratchTrialClassification::in_subset && planned.plan,
            "JSON spelling does not change decoded field identities");
    same_manifest(planned.plan->tables, enumerate(domain("L")));
    // Deep additive data uses setup workspace, never C++ recursion or an
    // arbitrary depth/receipt-size admission tier.
    json.insert(1, "\"nested\":" + std::string(128, '[') + "null" + std::string(128, ']') + ",");
    const auto deep = plan_scratch_trial(config, {}, false);
    require(deep.classification == ScratchTrialClassification::in_subset && deep.plan,
            "deep but valid unrelated receipt data is skipped");
}

void stream_binding_precedes_warmup() {
    Probe valid;
    require(valid.set_scratch_trial(trial(domain("stream"))) == 0, "install stream certificate");
    {
        FilelessScope fileless;
        require(valid.stream_begin(bars.data(), static_cast<int>(bars.size()), "1", "1"),
                "certified stream begins");
        valid.stream_end();
    }
    require(valid.last_error().empty() && valid.prepares == 1 && valid.begins == 1
            && valid.calculations >= static_cast<int>(bars.size()), "certified warmup actually ran");
    same_manifest(valid.observed->manifest(), enumerate(domain("stream")));
    Probe out;
    auto config = trial(domain("stream"));
    config.order_shapes_receipt = document(certificate(domain("stream")), "out_of_subset");
    require(out.set_scratch_trial(config) == 0, "install stream OUT");
    {
        FilelessScope fileless;
        require(!out.stream_begin(bars.data(), static_cast<int>(bars.size()), "1", "1"),
                "OUT stream does not begin");
    }
    require(out.scratch_trial_binding().classification
                == ScratchTrialClassification::needs_file_capable_worker
            && out.prepares == 0 && out.begins == 0 && out.calculations == 0,
            "stream OUT is returned before warmup bar zero");
}
} // namespace

int main() {
    try {
        receipt_absent();
        receipt_malformed();
        receipt_unknown_version();
        receipt_incomplete_roster();
        receipt_out_of_subset();
        in_subset_binds_derived_plan_and_runs();
        all_zero_certificate_is_not_p0();
        per_trial_binding();
        effective_inputs_and_missing_live_proofs_route_out();
        nonzero_root_plans_wait_for_the_root_binder();
        json_escaping_and_additive_fields();
        stream_binding_precedes_warmup();
        std::puts("test_scratch_trial: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_scratch_trial: %s\n", error.what());
        return 1;
    }
}
