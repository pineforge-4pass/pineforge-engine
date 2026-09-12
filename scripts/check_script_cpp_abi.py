#!/usr/bin/env python3
"""Compile/link-only checks for the internal generated/native C++ pairing.

Exact base38 headers are a frozen fixture: no Git history or network is needed.
Every translation unit must compile before expected linker failures are tested.
Neither a strategy nor any produced executable is run. C ABI checks are separate.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


BASE_COMMIT = "38dc73e5503fe5395458e5f8df2a2ad78054a1ae"
BASE_ENGINE_SHA256 = "06c937a1ccd31815ca7775268ac699ffdfddb1a1f19de4628b777f37e9a6d193"
CURRENT_NAMESPACE = "engine_script_run_v13"
BASE_NAMESPACE = "engine_script_run_v2"
V8_COMMIT = "79921099a9357cb5bbace907a9319479f6640d89"
V8_TREE = "e141657c572b4a3855dfee607f9951e331b961d6"
V8_ENGINE_SHA256 = "571c7b328ef86915c63523d066ce2761cfc361b4de413b7669ebe17f4fd30ad3"
V9_COMMIT = "f2df706062ee0509c499b5c757a8e1ac83fccec6"
V9_TREE = "3ae04a5eec3f8eb58a05ac48f994a2c6d0ab9579"
V9_ENGINE_SHA256 = "bb984f4c25a01470c147de1366aa8c715b170361be63c68e2db0cfdf09650e1a"
V10_COMMIT = "fd4c68685ebd612397e42bced731d7b7ee06c1b7"
V10_TREE = "24fb6e11d979c737561c5ce8a657c5ff703fc01e"
V10_ENGINE_SHA256 = "53b2cc6deb96143afe572581d92f0c5d8d0f67d83e307153be4b67798f4db998"
V12_COMMIT = "e7d023dbdff1c98229155ec5bcdd1e4ac534f5fb"
V12_TREE = "0201bf052429490fb453bbfd6037e5afd1669626"
V12_ENGINE_SHA256 = "210a43d3166e9788dfa8bb0e670776a243af136ef8fd026f340b52911484c8bf"
FIXTURE = Path(__file__).resolve().parents[1] / "tests/fixtures/script_cpp_abi/base38"


def entry_diagnostic(lines, namespace, method):
    # "run" also occurs inside engine_script_run_vN. Match the qualified
    # method itself, so a missing fill_report cannot masquerade as missing run.
    owner = "pineforge::" + (namespace + "::" if namespace else "")
    needle = owner + "BacktestEngine::" + method + "("
    return next((line for line in lines if needle in line), None)


def frozen_headers(destination, fixture=FIXTURE, commit=BASE_COMMIT,
                   namespace=BASE_NAMESPACE, engine_sha=BASE_ENGINE_SHA256, tree=None):
    """Authenticate and unpack the exact tracked base38 header closure."""
    manifest = json.loads((fixture / "manifest.json").read_text())
    if (manifest["source_commit"] != commit
            or manifest["internal_namespace"] != namespace
            or manifest["files"]["pineforge/engine.hpp"]["sha256"] != engine_sha
            or (tree is not None and manifest.get("source_tree") != tree)):
        raise RuntimeError("stale-header fixture does not identify the pinned contract")
    archive = (fixture / "headers.json.gz").read_bytes()
    if hashlib.sha256(archive).hexdigest() != manifest["archive_sha256"]:
        raise RuntimeError("stale-header fixture archive digest mismatch")
    contents = json.loads(gzip.decompress(archive))
    if contents.keys() != manifest["files"].keys():
        raise RuntimeError("stale-header fixture file set mismatch")
    for name, content in contents.items():
        relative = Path(name)
        if relative.is_absolute() or ".." in relative.parts or relative.parts[0] != "pineforge":
            raise RuntimeError("invalid stale-header fixture path: " + name)
        raw = content.encode()
        blob = b"blob " + str(len(raw)).encode() + b"\0" + raw
        expected = manifest["files"][name]
        if (hashlib.sha256(raw).hexdigest() != expected["sha256"]
                or hashlib.sha1(blob).hexdigest() != expected["git_blob"]):
            raise RuntimeError("stale-header fixture digest mismatch: " + name)
        path = destination / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(raw)


def caller(namespace, generated=False):
    header = f'''#include <pineforge/engine.hpp>
#include <type_traits>
static_assert(std::is_same<pineforge::BacktestEngine,
              pineforge::{namespace}::BacktestEngine>::value,
              "unexpected internal C++ namespace");
'''
    if generated:
        # Shape of supported codegen c8ffe587 emit_top.py's entry wrappers.
        # No Pine source is compiled and no GeneratedStrategy is instantiated.
        return header + '''
class GeneratedStrategy final : public pineforge::BacktestEngine {
    void on_bar(const pineforge::Bar&) override {}
    void prepare_script_run(const pineforge::Bar*, int, bool) override {}
};
extern "C" void pairing_generated_run(void* handle, pineforge::Bar* bars,
                                      int count, pineforge::ReportC* report) {
    auto* strategy = static_cast<GeneratedStrategy*>(handle);
    strategy->run(bars, count);
    strategy->run(bars, count, "", "", false, 4,
                  pineforge::MagnifierDistribution::ENDPOINTS);
    strategy->fill_report(report);
}
int main(int argc, char** argv) {
    pairing_generated_run(argv, nullptr, argc, nullptr);
    return 0;
}
'''
    return header + '''
int main(int argc, char** argv) {
    auto* strategy = reinterpret_cast<pineforge::BacktestEngine*>(argv);
    strategy->run(nullptr, argc);
    strategy->run(nullptr, argc, "", "", {}, pineforge::SymInfo{});
    strategy->fill_report(nullptr);
    return 0;
}
'''


def frozen_standalone_headers(destination):
    fixture = FIXTURE.parent.parent / "aggregate_cpp_abi/unversioned-draft"
    manifest = json.loads((fixture / "manifest.json").read_text())
    if manifest["base_commit"] != "cc0b22d0ede0f5fc35f54f2966284c68a6750a30":
        raise RuntimeError("standalone draft fixture has wrong base provenance")
    for name, expected in manifest["files"].items():
        relative = Path(name)
        if relative.is_absolute() or ".." in relative.parts or relative.parts[0] != "pineforge":
            raise RuntimeError("invalid standalone draft path")
        raw = (fixture / relative).read_bytes()
        if hashlib.sha256(raw).hexdigest() != expected["sha256"] or len(raw) != expected["bytes"]:
            raise RuntimeError("standalone draft digest mismatch: " + name)
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(raw)


# Only old entry-point symbols, compiled against the exact frozen old header.
# This is a linker control, NOT a historical runtime or economic simulation.
BASE_SYMBOL_CONTROL = '''#include <pineforge/engine.hpp>
namespace pineforge { namespace engine_script_run_v2 {
void BacktestEngine::run(const Bar*, int) {}
void BacktestEngine::run(const Bar*, int, const std::string&, const std::string&,
                        bool, int, MagnifierDistribution) {}
void BacktestEngine::run(const Bar*, int, const std::string&, const std::string&,
                        const std::unordered_map<std::string, std::string>&,
                        const SymInfo&, const StrategyOverrides*, bool, int,
                        MagnifierDistribution) {}
void BacktestEngine::fill_report(ReportC*) const {}
}}
'''
LEGACY_CALLER = '''namespace pineforge {
struct Bar;
class BacktestEngine { public: void run(const Bar*, int); };
}
int main(int argc, char** argv) {
    auto* strategy = reinterpret_cast<pineforge::BacktestEngine*>(argv);
    strategy->run(nullptr, argc);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--library", required=True)
    parser.add_argument("--include", required=True)
    parser.add_argument("--generated-include", required=True)
    parser.add_argument("--extra-flag", action="append", default=[])
    parser.add_argument("--receipt", type=Path)
    args = parser.parse_args()
    from check_aggregate_cpp_versions import check as check_aggregate_versions
    check_aggregate_versions(Path(args.include).resolve().parent)
    receipt = {"library_sha256": hashlib.sha256(Path(args.library).read_bytes()).hexdigest(),
               "current_namespace": CURRENT_NAMESPACE,
               "standalone_namespace": "reservation_expansion_v1",
               "standalone_namespaces": {"reservation": "reservation_expansion_v1",
                   "lifecycle": "pineforge::exit_legs::lifecycle_v1",
                   "admission": "pineforge::admission::market_admission_v2",
                   "cancellation": "pineforge::order_cancellation_v1"},
               "executable_runs": 0, "compiles": [], "links": []}
    # Literal diagnostic controls guard the link-failure parser itself.
    for namespace in (BASE_NAMESPACE, CURRENT_NAMESPACE):
        report_only = [f"undefined pineforge::{namespace}::BacktestEngine::fill_report(pineforge::ReportC*) const"]
        if (entry_diagnostic(report_only, namespace, "run") is not None
                or entry_diagnostic(report_only, namespace, "fill_report") is None):
            raise RuntimeError("link diagnostics confuse namespace and method names")
    print("qualified-method diagnostic controls passed")
    with tempfile.TemporaryDirectory(prefix="pf-script-cpp-abi-") as temporary:
        root = Path(temporary)
        old_include = root / "base38/include"
        frozen_headers(old_include)
        cap_include = root / "basef864/include"
        frozen_headers(cap_include, FIXTURE.parent / "basef864",
                       "f864be590931ba08c8df5af983b33b2c29be9c67", "engine_script_run_v3",
                       "54b35fffaa163a31467f8ba883e44e02f013a37216b558dfe3a4f28fbbe84dc2")
        prior_include = root / "basec45/include"
        frozen_headers(prior_include, FIXTURE.parent / "basec45",
                       "c45cf5a4d0e67a2ac098d9066977e1fa21c408a9", "engine_script_run_v4",
                       "3b4e2937a9b5f275dd119144373b1bf15e433092009500092cd32ea34963b293")
        activation_include = root / "base149/include"
        frozen_headers(activation_include, FIXTURE.parent / "base149",
                       "149f77ce16ef84c6da77e67d812bf8fa88e51cde", "engine_script_run_v5",
                       "5ba773889d947e4fdab3995cc55f22f88ab037a86e0ad4016a126d297ce82eed")
        shipped_include = root / "baseff54/include"
        frozen_headers(shipped_include, FIXTURE.parent / "baseff54",
                       "ff54a557ac751244dafd60df0bb22886ec35792d", "engine_script_run_v6",
                       "d5d74b2b0542ce7aa2bf3e0a95bac8d05318ce15494148f4f07f92c7e332b237",
                       "60431e5da18d4bce0777f0e3b4df03d41163c350")
        growth_include = root / "growthbf312/include"
        frozen_headers(growth_include, FIXTURE.parent / "growthbf312",
                       "ff54a557ac751244dafd60df0bb22886ec35792d", "engine_script_run_v6",
                       "381a18d59f20ff94c6eed9dec40497fdaa175637fc96169a0a9875a38b071736",
                       "bf312b9d5a705d3d0ca16d4fb897e16c6e73b0d1")
        cc0_include = root / "basecc0/include"
        frozen_headers(cc0_include, FIXTURE.parent / "basecc0",
                       "cc0b22d0ede0f5fc35f54f2966284c68a6750a30", "engine_script_run_v7",
                       "bc86697bbdb229f65a975d810c8b4d7a98db3062028d03d180e6f8fea6bf7c4d",
                       "3b33cd3c37e1ed34e3ac2d77a2a8ddb6ce9aebe1")
        v8_include = root / "basev8/include"
        frozen_headers(v8_include, FIXTURE.parent / "basev8", V8_COMMIT,
                       "engine_script_run_v8", V8_ENGINE_SHA256, V8_TREE)
        v9_include = root / "basev9/include"
        frozen_headers(v9_include, FIXTURE.parent / "basev9", V9_COMMIT,
                       "engine_script_run_v9", V9_ENGINE_SHA256, V9_TREE)
        v10_include = root / "basev10/include"
        frozen_headers(v10_include, FIXTURE.parent / "basev10", V10_COMMIT,
                       "engine_script_run_v10", V10_ENGINE_SHA256, V10_TREE)
        v12_include = root / "basev12/include"
        frozen_headers(v12_include, FIXTURE.parent / "basev12", V12_COMMIT,
                       "engine_script_run_v12", V12_ENGINE_SHA256, V12_TREE)
        standalone_draft_include = root / "standalone-draft/include"
        frozen_standalone_headers(standalone_draft_include)
        # The exact pre-v11 aggregate closure supplies the frozen
        # market_admission_v1 surface. The opposite-intent implementation
        # promotes this standalone surface to v2 when BookObservation gains
        # its raw requested direction.
        admission_v1_include = root / "basev10-admission-v1/include"
        frozen_headers(admission_v1_include, FIXTURE.parent / "basev10",
                       V10_COMMIT, "engine_script_run_v10", V10_ENGINE_SHA256, V10_TREE)
        common = [args.compiler, "-std=c++17", "-O0", *args.extra_flag]

        def compile_object(name, source, include):
            path = root / (name + ".cpp")
            path.write_text(source)
            obj = root / (name + ".o")
            compiled = subprocess.run(
                [*common, "-I", str(include), "-I", args.generated_include,
                 "-c", str(path), "-o", str(obj)],
                capture_output=True, text=True, timeout=60,
            )
            if compiled.returncode:
                raise RuntimeError(name + " failed to compile (not a pairing rejection):\n"
                                   + compiled.stderr)
            receipt["compiles"].append({"name": name, "exit": compiled.returncode,
                "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
                "object_sha256": hashlib.sha256(obj.read_bytes()).hexdigest()})
            return obj

        current_native = compile_object("current_native", caller(CURRENT_NAMESPACE), args.include)
        current_generated = compile_object("current_generated", caller(CURRENT_NAMESPACE, True), args.include)
        stale_native = compile_object("base38_native", caller(BASE_NAMESPACE), old_include)
        stale_generated = compile_object("base38_generated", caller(BASE_NAMESPACE, True), old_include)
        old_symbols = compile_object("base38_symbol_control", BASE_SYMBOL_CONTROL, old_include)
        legacy = compile_object("legacy_unversioned", LEGACY_CALLER, old_include)

        cap_native = compile_object("basef864_native", caller("engine_script_run_v3"), cap_include)
        cap_generated = compile_object("basef864_generated", caller("engine_script_run_v3", True), cap_include)
        cap_symbols = compile_object("basef864_symbol_control",
            BASE_SYMBOL_CONTROL.replace("engine_script_run_v2", "engine_script_run_v3"), cap_include)

        prior_native = compile_object("basec45_native", caller("engine_script_run_v4"), prior_include)
        prior_generated = compile_object("basec45_generated", caller("engine_script_run_v4", True), prior_include)
        prior_symbols = compile_object("basec45_symbol_control",
            BASE_SYMBOL_CONTROL.replace("engine_script_run_v2", "engine_script_run_v4"), prior_include)

        priority_caller = """#include <pineforge/engine.hpp>
#include <pineforge/compat/pine/order_priority.hpp>
int main() {
    pineforge::compat::pine::OrderPriority policy;
    policy.attach();
    pineforge::compat::pine::OrderPriorityContext context{};
    std::vector<pineforge::PendingOrder> orders(2);
    return policy.select(context, orders).has_value() ? 1 : 0;
}
"""
        priority_symbols = """#include <pineforge/engine.hpp>
#include <pineforge/compat/pine/order_priority.hpp>
namespace pineforge::compat::pine {
std::optional<broker::OrderPriorityDecision> OrderPriority::select(
    const OrderPriorityContext&, const std::vector<PendingOrder>&) const { return std::nullopt; }
}
"""
        current_priority = compile_object("current_pending_priority", priority_caller, args.include)
        prior_priority = compile_object("basec45_pending_priority", priority_caller, prior_include)
        prior_priority_symbols = compile_object("basec45_pending_priority_symbols", priority_symbols, prior_include)

        activation_native = compile_object("base149_native", caller("engine_script_run_v5"), activation_include)
        activation_generated = compile_object("base149_generated", caller("engine_script_run_v5", True), activation_include)
        activation_symbols = compile_object("base149_symbol_control",
            BASE_SYMBOL_CONTROL.replace("engine_script_run_v2", "engine_script_run_v5"), activation_include)
        activation_priority = compile_object("base149_pending_priority", priority_caller, activation_include)
        activation_priority_symbols = compile_object("base149_pending_priority_symbols", priority_symbols, activation_include)
        shipped_native = compile_object("baseff54_native", caller("engine_script_run_v6"), shipped_include)
        shipped_generated = compile_object("baseff54_generated", caller("engine_script_run_v6", True), shipped_include)
        shipped_symbols = compile_object("baseff54_symbol_control",
            BASE_SYMBOL_CONTROL.replace("engine_script_run_v2", "engine_script_run_v6"), shipped_include)
        shipped_priority = compile_object("baseff54_pending_priority", priority_caller, shipped_include)
        shipped_priority_symbols = compile_object("baseff54_pending_priority_symbols", priority_symbols, shipped_include)
        cc0_native = compile_object("basecc0_native", caller("engine_script_run_v7"), cc0_include)
        cc0_generated = compile_object("basecc0_generated", caller("engine_script_run_v7", True), cc0_include)
        cc0_symbols = compile_object("basecc0_symbol_control",
            BASE_SYMBOL_CONTROL.replace("engine_script_run_v2", "engine_script_run_v7"), cc0_include)
        cc0_priority = compile_object("basecc0_pending_priority", priority_caller, cc0_include)
        cc0_priority_symbols = compile_object("basecc0_pending_priority_symbols", priority_symbols, cc0_include)
        v8_native = compile_object("basev8_native", caller("engine_script_run_v8"), v8_include)
        v8_generated = compile_object("basev8_generated", caller("engine_script_run_v8", True), v8_include)
        v8_symbols = compile_object("basev8_symbol_control",
            BASE_SYMBOL_CONTROL.replace("engine_script_run_v2", "engine_script_run_v8"), v8_include)
        v8_priority = compile_object("basev8_pending_priority", priority_caller, v8_include)
        v8_priority_symbols = compile_object("basev8_pending_priority_symbols", priority_symbols, v8_include)
        v9_native = compile_object("basev9_native", caller("engine_script_run_v9"), v9_include)
        v9_generated = compile_object("basev9_generated", caller("engine_script_run_v9", True), v9_include)
        v9_symbols = compile_object("basev9_symbol_control",
            BASE_SYMBOL_CONTROL.replace("engine_script_run_v2", "engine_script_run_v9"), v9_include)
        v9_priority = compile_object("basev9_pending_priority", priority_caller, v9_include)
        v9_priority_symbols = compile_object("basev9_pending_priority_symbols", priority_symbols, v9_include)
        v10_native = compile_object("basev10_native", caller("engine_script_run_v10"), v10_include)
        v10_generated = compile_object("basev10_generated", caller("engine_script_run_v10", True), v10_include)
        v10_symbols = compile_object("basev10_symbol_control",
            BASE_SYMBOL_CONTROL.replace("engine_script_run_v2", "engine_script_run_v10"), v10_include)
        v10_priority = compile_object("basev10_pending_priority", priority_caller, v10_include)
        v10_priority_symbols = compile_object("basev10_pending_priority_symbols", priority_symbols, v10_include)
        v12_native = compile_object("basev12_native", caller("engine_script_run_v12"), v12_include)
        v12_generated = compile_object("basev12_generated", caller("engine_script_run_v12", True), v12_include)
        # V12 moved its virtual destructor out of line. Its positive linker
        # control must define that key function so UBSan vptr instrumentation
        # can resolve the class RTTI. This remains a never-executed symbol
        # control compiled against the unchanged historical header.
        v12_symbol_control = BASE_SYMBOL_CONTROL.replace("engine_script_run_v2", "engine_script_run_v12") + '''
namespace pineforge { namespace engine_script_run_v12 {
BacktestEngine::~BacktestEngine() = default;
}}
'''
        v12_symbols = compile_object("basev12_symbol_control",
            v12_symbol_control, v12_include)
        v12_priority = compile_object("basev12_pending_priority", priority_caller, v12_include)
        v12_priority_symbols = compile_object("basev12_pending_priority_symbols", priority_symbols, v12_include)

        reservation_caller = '''#include <pineforge/reservation_expansion.hpp>
int main(int argc, char**) {
    pineforge::ReservationExpansion expansion;
    pineforge::ReservationGrowthSource source;
    const auto side = static_cast<pineforge::PositionSide>(argc);
    double capacity = 4.0;
    expansion.capture(1, 1, side, capacity);
    expansion.close_population(2);
    expansion.grow(capacity, 1, side, 1.0, 1, side, 2.0, 1e-10);
    source.assign_capture(3, 1);
    return expansion.owns_exposure(1, side) ? 0 : 1;
}
'''
        reservation_symbols = '''#include <pineforge/reservation_expansion.hpp>
namespace pineforge {
void ReservationExpansion::capture(uint64_t, int64_t, PositionSide, double) {}
void ReservationExpansion::close_population(uint64_t) {}
bool ReservationExpansion::owns_exposure(int64_t, PositionSide) const { return false; }
void ReservationExpansion::grow(double&, int64_t, PositionSide, double,
    int64_t, PositionSide, double, double) const {}
void ReservationGrowthSource::assign_capture(uint64_t, uint64_t) {}
}
'''
        reservation_assertions = '''#include <type_traits>
static_assert(std::is_same<pineforge::ReservationExpansion,
    pineforge::reservation_expansion_v1::ReservationExpansion>::value);
static_assert(std::is_same<pineforge::ReservationExpansionCapture,
    pineforge::reservation_expansion_v1::ReservationExpansionCapture>::value);
static_assert(std::is_same<pineforge::ReservationGrowthSource,
    pineforge::reservation_expansion_v1::ReservationGrowthSource>::value);
'''
        current_reservation = compile_object("current_standalone_reservation",
            reservation_caller + reservation_assertions, args.include)
        draft_reservation = compile_object("growthbf312_unversioned_reservation", reservation_caller, growth_include)
        draft_reservation_symbols = compile_object("growthbf312_reservation_symbols", reservation_symbols, growth_include)
        capture_caller = '''#include <pineforge/reservation_expansion.hpp>
void pairing_capture(const pineforge::ReservationExpansionCapture&);
int main() {
    pineforge::ReservationExpansionCapture capture{1, static_cast<pineforge::PositionSide>(1), {}};
    pairing_capture(capture);
}
'''
        capture_provider = '''#include <pineforge/reservation_expansion.hpp>
void pairing_capture(const pineforge::ReservationExpansionCapture&) {}
'''
        current_capture = compile_object("current_capture_argument", capture_caller, args.include)
        draft_capture = compile_object("growthbf312_capture_argument", capture_caller, growth_include)
        current_capture_symbols = compile_object("current_capture_symbols", capture_provider, args.include)
        draft_capture_symbols = compile_object("growthbf312_capture_symbols", capture_provider, growth_include)

        lifecycle_provider = '''#include <pineforge/exit_leg_lifecycle.hpp>
void pairing_lifecycle(const pineforge::exit_legs::Lifecycle&,
    const pineforge::exit_legs::Action&, const pineforge::exit_legs::Frame&,
    const pineforge::exit_legs::Definition&) {}
'''
        lifecycle_caller = '''#include <pineforge/exit_leg_lifecycle.hpp>
void pairing_lifecycle(const pineforge::exit_legs::Lifecycle&,
    const pineforge::exit_legs::Action&, const pineforge::exit_legs::Frame&,
    const pineforge::exit_legs::Definition&);
int main() {
    pineforge::exit_legs::Lifecycle life;
    pineforge::exit_legs::Action action{};
    pineforge::exit_legs::Frame frame{};
    pairing_lifecycle(life, action, frame, life.current_definition());
}
'''
        lifecycle_assertion = '''#include <type_traits>
static_assert(std::is_same_v<pineforge::exit_legs::Lifecycle,
    pineforge::exit_legs::lifecycle_v1::Lifecycle>);
static_assert(std::is_same_v<pineforge::exit_legs::Action,
    pineforge::exit_legs::lifecycle_v1::Action>);
'''
        current_lifecycle = compile_object("current_lifecycle_argument", lifecycle_caller + lifecycle_assertion, args.include)
        draft_lifecycle = compile_object("draft_lifecycle_argument", lifecycle_caller, standalone_draft_include)
        current_lifecycle_symbols = compile_object("current_lifecycle_symbols", lifecycle_provider, args.include)
        draft_lifecycle_symbols = compile_object("draft_lifecycle_symbols", lifecycle_provider, standalone_draft_include)
        admission_caller = '''#include <pineforge/market_admission.hpp>
#include <utility>
int main() {
    pineforge::admission::Draft draft;
    pineforge::admission::Journal journal;
    draft.bind(std::make_shared<const pineforge::admission::CommandObservation>());
    draft.reviewed({}); draft.sizing_revised({});
    journal.next_sequence(); journal.append(pineforge::admission::CommandEvent{});
    journal.retain({}); journal.reflect("", [](const pineforge::admission::Field&) {});
    auto allocation = journal.reserve();
    auto moved = std::move(allocation);
    journal.reset();
}
'''
        admission_assertion = '''#include <type_traits>
static_assert(std::is_same_v<pineforge::admission::Draft,
    pineforge::admission::market_admission_v2::Draft>);
static_assert(std::is_same_v<pineforge::admission::Journal,
    pineforge::admission::market_admission_v2::Journal>);
static_assert(std::is_same_v<pineforge::admission::Allocation,
    pineforge::admission::market_admission_v2::Allocation>);
static_assert(std::is_same_v<pineforge::admission::CommandCapture,
    pineforge::admission::market_admission_v2::CommandCapture>);
static_assert(std::is_same_v<pineforge::admission::ReviewCapture,
    pineforge::admission::market_admission_v2::ReviewCapture>);
'''
        admission_symbols = '''#include <pineforge/market_admission.hpp>
namespace pineforge::admission {
void Draft::bind(std::shared_ptr<const CommandObservation>) {}
void Draft::reviewed(ReviewReceipt) {}
void Draft::sizing_revised(SizingRevision) {}
uint64_t Journal::next_sequence() { return 1; }
Allocation Journal::reserve() { return Allocation(*this, 1); }
Allocation::Allocation(Allocation&& other) noexcept
    : journal_(other.journal_), sequence_(other.sequence_) { other.journal_ = nullptr; }
Allocation::~Allocation() noexcept {}
void Journal::append(Event) {}
void Journal::retain(const std::vector<uint64_t>&) {}
void Journal::reflect(const std::string&, const FieldVisitor&) const {}
void Journal::reset() {}
}
'''
        current_admission = compile_object("current_admission_methods", admission_caller + admission_assertion, args.include)
        draft_admission = compile_object("draft_admission_methods", admission_caller, standalone_draft_include)
        draft_admission_symbols = compile_object("draft_admission_symbols", admission_symbols, standalone_draft_include)
        v1_admission = compile_object("v1_admission_methods", admission_caller, admission_v1_include)
        v1_admission_symbols = compile_object("v1_admission_symbols", admission_symbols, admission_v1_include)

        cancellation_caller = '''#include <pineforge/order_cancellation.hpp>
#include <type_traits>
static_assert(std::is_same_v<pineforge::OrderCancellationReceipt,
    pineforge::order_cancellation_v1::OrderCancellationReceipt>);
static_assert(static_cast<int>(pineforge::CancellationCause::None) == 0);
static_assert(static_cast<int>(pineforge::CancellationCause::Replacement) == 1);
static_assert(static_cast<int>(pineforge::CancellationCause::Dependency) == 2);
void pairing_cancellation(const pineforge::OrderCancellationReceipt&);
int main() {
    pineforge::OrderCancellationReceipt receipt;
    pairing_cancellation(receipt);
    return receipt.cancelled() ? 1 : 0;
}
'''
        cancellation_provider = '''#include <pineforge/order_cancellation.hpp>
void pairing_cancellation(const pineforge::order_cancellation_v1::OrderCancellationReceipt&) {}
'''
        current_cancellation = compile_object("current_standalone_cancellation",
            cancellation_caller, args.include)
        current_cancellation_symbols = compile_object("current_standalone_cancellation_symbols",
            cancellation_provider, args.include)

        def link(name, obj, runtime, missing_namespace=None):
            linked = subprocess.run(
                [*common, str(obj), str(runtime), "-pthread", "-o", str(root / name)],
                capture_output=True, text=True, timeout=60,
            )
            if missing_namespace is None:
                if linked.returncode:
                    raise RuntimeError(name + " positive control failed to link:\n" + linked.stderr)
                print(name + ": linked (not executed)")
                receipt["links"].append({"name": name, "outcome": "linked", "exit": 0})
                return
            if not linked.returncode:
                raise RuntimeError(name + " stale C++ pairing unexpectedly linked")
            # Require the missing engine entry symbol, not an arbitrary linker
            # failure (missing library, compiler flags, unrelated dependency).
            required_methods = ("run", "fill_report") if missing_namespace else ("run",)
            entries = {method: entry_diagnostic(linked.stderr.splitlines(), missing_namespace, method)
                       for method in required_methods}
            if ("undefined" not in linked.stderr.lower()
                    or any(line is None for line in entries.values())):
                raise RuntimeError(name + " failed for an unexpected reason:\n" + linked.stderr)
            print(name + ": rejected missing pineforge::"
                  + (missing_namespace + "::" if missing_namespace else "")
                  + "BacktestEngine entry symbols")
            for method in required_methods:
                print("  " + entries[method].strip())
            receipt["links"].append({"name": name, "outcome": "expected_rejection",
                "exit": linked.returncode, "diagnostics": list(entries.values())})

        link("current_native_to_current", current_native, args.library)
        link("current_generated_to_current", current_generated, args.library)
        link("base38_native_to_v2_symbol_control", stale_native, old_symbols)
        link("base38_generated_to_v2_symbol_control", stale_generated, old_symbols)
        link("base38_native_to_current", stale_native, args.library, BASE_NAMESPACE)
        link("base38_generated_to_current", stale_generated, args.library, BASE_NAMESPACE)
        link("current_native_to_v2_symbol_control", current_native, old_symbols, CURRENT_NAMESPACE)
        link("current_generated_to_v2_symbol_control", current_generated, old_symbols, CURRENT_NAMESPACE)
        link("unversioned_to_current", legacy, args.library, "")
        link("basef864_native_to_v3_symbol_control", cap_native, cap_symbols)
        link("basef864_generated_to_v3_symbol_control", cap_generated, cap_symbols)
        link("basef864_native_to_current", cap_native, args.library, "engine_script_run_v3")
        link("basef864_generated_to_current", cap_generated, args.library, "engine_script_run_v3")
        link("current_native_to_v3_symbol_control", current_native, cap_symbols, CURRENT_NAMESPACE)
        link("current_generated_to_v3_symbol_control", current_generated, cap_symbols, CURRENT_NAMESPACE)
        link("basec45_native_to_v4_symbol_control", prior_native, prior_symbols)
        link("basec45_generated_to_v4_symbol_control", prior_generated, prior_symbols)
        link("basec45_native_to_current", prior_native, args.library, "engine_script_run_v4")
        link("basec45_generated_to_current", prior_generated, args.library, "engine_script_run_v4")
        link("current_native_to_v4_symbol_control", current_native, prior_symbols, CURRENT_NAMESPACE)
        link("current_generated_to_v4_symbol_control", current_generated, prior_symbols, CURRENT_NAMESPACE)
        link("current_pending_priority_to_current", current_priority, args.library)
        link("basec45_pending_priority_to_v4_symbols", prior_priority, prior_priority_symbols)
        link("base149_native_to_v5_symbol_control", activation_native, activation_symbols)
        link("base149_generated_to_v5_symbol_control", activation_generated, activation_symbols)
        link("base149_native_to_current", activation_native, args.library, "engine_script_run_v5")
        link("base149_generated_to_current", activation_generated, args.library, "engine_script_run_v5")
        link("current_native_to_v5_symbol_control", current_native, activation_symbols, CURRENT_NAMESPACE)
        link("current_generated_to_v5_symbol_control", current_generated, activation_symbols, CURRENT_NAMESPACE)
        link("base149_pending_priority_to_v5_symbols", activation_priority, activation_priority_symbols)
        link("baseff54_native_to_v6_symbols", shipped_native, shipped_symbols)
        link("baseff54_generated_to_v6_symbols", shipped_generated, shipped_symbols)
        link("baseff54_native_to_current", shipped_native, args.library, "engine_script_run_v6")
        link("baseff54_generated_to_current", shipped_generated, args.library, "engine_script_run_v6")
        link("current_native_to_v6_symbols", current_native, shipped_symbols, CURRENT_NAMESPACE)
        link("current_generated_to_v6_symbols", current_generated, shipped_symbols, CURRENT_NAMESPACE)
        link("baseff54_pending_priority_to_v6_symbols", shipped_priority, shipped_priority_symbols)
        link("basecc0_native_to_v7_symbols", cc0_native, cc0_symbols)
        link("basecc0_generated_to_v7_symbols", cc0_generated, cc0_symbols)
        link("basecc0_native_to_current", cc0_native, args.library, "engine_script_run_v7")
        link("basecc0_generated_to_current", cc0_generated, args.library, "engine_script_run_v7")
        link("current_native_to_v7_symbols", current_native, cc0_symbols, CURRENT_NAMESPACE)
        link("current_generated_to_v7_symbols", current_generated, cc0_symbols, CURRENT_NAMESPACE)
        link("basecc0_pending_priority_to_v7_symbols", cc0_priority, cc0_priority_symbols)
        link("basev8_native_to_v8_symbols", v8_native, v8_symbols)
        link("basev8_generated_to_v8_symbols", v8_generated, v8_symbols)
        link("basev8_native_to_current", v8_native, args.library, "engine_script_run_v8")
        link("basev8_generated_to_current", v8_generated, args.library, "engine_script_run_v8")
        link("current_native_to_v8_symbols", current_native, v8_symbols, CURRENT_NAMESPACE)
        link("current_generated_to_v8_symbols", current_generated, v8_symbols, CURRENT_NAMESPACE)
        link("basev8_pending_priority_to_v8_symbols", v8_priority, v8_priority_symbols)
        link("basev9_native_to_v9_symbols", v9_native, v9_symbols)
        link("basev9_generated_to_v9_symbols", v9_generated, v9_symbols)
        link("basev9_native_to_current", v9_native, args.library, "engine_script_run_v9")
        link("basev9_generated_to_current", v9_generated, args.library, "engine_script_run_v9")
        link("current_native_to_v9_symbols", current_native, v9_symbols, CURRENT_NAMESPACE)
        link("current_generated_to_v9_symbols", current_generated, v9_symbols, CURRENT_NAMESPACE)
        link("basev9_pending_priority_to_v9_symbols", v9_priority, v9_priority_symbols)
        link("basev10_native_to_v10_symbols", v10_native, v10_symbols)
        link("basev10_generated_to_v10_symbols", v10_generated, v10_symbols)
        link("basev10_native_to_current", v10_native, args.library, "engine_script_run_v10")
        link("basev10_generated_to_current", v10_generated, args.library, "engine_script_run_v10")
        link("current_native_to_v10_symbols", current_native, v10_symbols, CURRENT_NAMESPACE)
        link("current_generated_to_v10_symbols", current_generated, v10_symbols, CURRENT_NAMESPACE)
        link("basev10_pending_priority_to_v10_symbols", v10_priority, v10_priority_symbols)
        link("basev12_native_to_v12_symbols", v12_native, v12_symbols)
        link("basev12_generated_to_v12_symbols", v12_generated, v12_symbols)
        link("basev12_native_to_current", v12_native, args.library, "engine_script_run_v12")
        link("basev12_generated_to_current", v12_generated, args.library, "engine_script_run_v12")
        link("current_native_to_v12_symbols", current_native, v12_symbols, CURRENT_NAMESPACE)
        link("current_generated_to_v12_symbols", current_generated, v12_symbols, CURRENT_NAMESPACE)
        link("basev12_pending_priority_to_v12_symbols", v12_priority, v12_priority_symbols)
        for name, obj, runtime, expected in [
            ("basecc0_pending_priority_to_current", cc0_priority, args.library, "pineforge::engine_script_run_v7::PendingOrder"),
            ("current_pending_priority_to_v7_symbols", current_priority, cc0_priority_symbols, "pineforge::engine_script_run_v13::PendingOrder"),
            ("baseff54_pending_priority_to_current", shipped_priority, args.library, "pineforge::engine_script_run_v6::PendingOrder"),
            ("current_pending_priority_to_v6_symbols", current_priority, shipped_priority_symbols, "pineforge::engine_script_run_v13::PendingOrder"),
            ("base149_pending_priority_to_current", activation_priority, args.library, "pineforge::engine_script_run_v5::PendingOrder"),
            ("current_pending_priority_to_v5_symbols", current_priority, activation_priority_symbols, "pineforge::engine_script_run_v13::PendingOrder"),
            ("basec45_pending_priority_to_current", prior_priority, args.library, "pineforge::PendingOrder"),
            ("current_pending_priority_to_v4_symbols", current_priority, prior_priority_symbols,
             "pineforge::engine_script_run_v13::PendingOrder"),
            ("basev8_pending_priority_to_current", v8_priority, args.library,
             "pineforge::engine_script_run_v8::PendingOrder"),
            ("current_pending_priority_to_v8_symbols", current_priority, v8_priority_symbols,
             "pineforge::engine_script_run_v13::PendingOrder"),
            ("basev9_pending_priority_to_current", v9_priority, args.library,
             "pineforge::engine_script_run_v9::PendingOrder"),
            ("current_pending_priority_to_v9_symbols", current_priority, v9_priority_symbols,
             "pineforge::engine_script_run_v13::PendingOrder"),
            ("basev10_pending_priority_to_current", v10_priority, args.library,
             "pineforge::engine_script_run_v10::PendingOrder"),
            ("current_pending_priority_to_v10_symbols", current_priority, v10_priority_symbols,
             "pineforge::engine_script_run_v13::PendingOrder"),
            ("basev12_pending_priority_to_current", v12_priority, args.library,
             "pineforge::engine_script_run_v12::PendingOrder"),
            ("current_pending_priority_to_v12_symbols", current_priority, v12_priority_symbols,
             "pineforge::engine_script_run_v13::PendingOrder"),
        ]:
            result = subprocess.run([*common, str(obj), str(runtime), "-pthread", "-o", str(root / name)],
                                    capture_output=True, text=True, timeout=60)
            if (result.returncode == 0 or "undefined" not in result.stderr.lower()
                    or "OrderPriority::select(" not in result.stderr or expected not in result.stderr):
                raise RuntimeError(name + " did not reject the expected PendingOrder type: " + result.stderr)
            print(name + ": rejected stale standalone PendingOrder argument type (not executed)")
            receipt["links"].append({"name": name, "outcome": "expected_rejection",
                "exit": result.returncode, "diagnostics": result.stderr})

        link("current_lifecycle_to_current_symbols", current_lifecycle, current_lifecycle_symbols)
        link("draft_lifecycle_to_draft_symbols", draft_lifecycle, draft_lifecycle_symbols)
        link("current_admission_to_current", current_admission, args.library)
        link("draft_admission_to_draft_symbols", draft_admission, draft_admission_symbols)
        link("v1_admission_to_v1_symbols", v1_admission, v1_admission_symbols)
        for name, obj, runtime, expected in [
            ("draft_lifecycle_to_current_symbols", draft_lifecycle, current_lifecycle_symbols,
             ["pairing_lifecycle(pineforge::exit_legs::Lifecycle const&"]),
            ("current_lifecycle_to_draft_symbols", current_lifecycle, draft_lifecycle_symbols,
             ["pairing_lifecycle(pineforge::exit_legs::lifecycle_v1::Lifecycle const&"]),
            ("draft_admission_to_current", draft_admission, args.library,
             ["pineforge::admission::Draft::bind(", "pineforge::admission::Journal::next_sequence(",
              "pineforge::admission::Allocation::~Allocation("]),
            ("current_admission_to_draft_symbols", current_admission, draft_admission_symbols,
             ["pineforge::admission::market_admission_v2::Draft::bind(",
              "pineforge::admission::market_admission_v2::Journal::next_sequence(",
              "pineforge::admission::market_admission_v2::Allocation::~Allocation("]),
            ("v1_admission_to_current", v1_admission, args.library,
             ["pineforge::admission::market_admission_v1::Draft::bind(",
              "pineforge::admission::market_admission_v1::Journal::next_sequence(",
              "pineforge::admission::market_admission_v1::Allocation::~Allocation("]),
            ("current_admission_to_v1_symbols", current_admission, v1_admission_symbols,
             ["pineforge::admission::market_admission_v2::Draft::bind(",
              "pineforge::admission::market_admission_v2::Journal::next_sequence(",
              "pineforge::admission::market_admission_v2::Allocation::~Allocation("]),
        ]:
            result = subprocess.run([*common, str(obj), str(runtime), "-pthread", "-o", str(root / name)],
                                    capture_output=True, text=True, timeout=60)
            if (result.returncode == 0 or "undefined" not in result.stderr.lower()
                    or any(needle not in result.stderr for needle in expected)):
                raise RuntimeError(name + " did not reject the expected aggregate standalone ABI: " + result.stderr)
            print(name + ": rejected stale standalone lifecycle/admission ABI (not executed)")
            receipt["links"].append({"name": name, "outcome": "expected_rejection",
                "exit": result.returncode, "diagnostics": result.stderr})

        link("current_cancellation_to_current_symbols",
             current_cancellation, current_cancellation_symbols)

        link("current_reservation_to_current", current_reservation, args.library)
        link("draft_reservation_to_draft_symbols", draft_reservation, draft_reservation_symbols)
        link("current_capture_to_current_symbols", current_capture, current_capture_symbols)
        link("draft_capture_to_draft_symbols", draft_capture, draft_capture_symbols)
        for name, obj, runtime, expected in [
            ("draft_reservation_to_current", draft_reservation, args.library,
             ["pineforge::ReservationExpansion::capture(", "pineforge::ReservationGrowthSource::assign_capture("]),
            ("current_reservation_to_draft_symbols", current_reservation, draft_reservation_symbols,
             ["pineforge::reservation_expansion_v1::ReservationExpansion::capture(",
              "pineforge::reservation_expansion_v1::ReservationGrowthSource::assign_capture("]),
            ("draft_capture_to_current_symbols", draft_capture, current_capture_symbols,
             ["pairing_capture(pineforge::ReservationExpansionCapture const&)"]),
            ("current_capture_to_draft_symbols", current_capture, draft_capture_symbols,
             ["pairing_capture(pineforge::reservation_expansion_v1::ReservationExpansionCapture const&)"]),
        ]:
            result = subprocess.run([*common, str(obj), str(runtime), "-pthread", "-o", str(root / name)],
                                    capture_output=True, text=True, timeout=60)
            if (result.returncode == 0 or "undefined" not in result.stderr.lower()
                    or any(needle not in result.stderr for needle in expected)):
                raise RuntimeError(name + " did not reject the expected reservation ABI: " + result.stderr)
            print(name + ": rejected stale standalone reservation ABI (not executed)")
            receipt["links"].append({"name": name, "outcome": "expected_rejection",
                "exit": result.returncode, "diagnostics": result.stderr})
    linked = sum(item["outcome"] == "linked" for item in receipt["links"])
    rejected = sum(item["outcome"] == "expected_rejection" for item in receipt["links"])
    receipt["summary"] = {"compiled": len(receipt["compiles"]), "linked": linked, "rejected": rejected}
    if args.receipt:
        args.receipt.write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"{len(receipt['compiles'])} translation units compiled; {linked} positive links; "
          f"{rejected} rejected links; no executable run")


if __name__ == "__main__":
    main()
