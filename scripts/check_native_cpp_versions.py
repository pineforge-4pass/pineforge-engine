#!/usr/bin/env python3
"""Source-only native C++ ABI ownership guard; no compiler or engine runs."""
from pathlib import Path
import re
import tempfile

from check_aggregate_cpp_versions import body, clean, standalone_scope
from prepare_settlement_cpp_abi_base import PROVIDERS, authenticate_headers, extract_tar

ROOT = Path(__file__).resolve().parents[1]
FILES = (
    "include/pineforge/native_order.hpp",
    "src/native_order.cpp",
    "include/pineforge/native_calendar.hpp",
    "src/native_calendar.cpp",
    "include/pineforge/native_run_spec.hpp",
    "src/native_run_spec.cpp",
    "include/pineforge/market_driver.hpp",
    "src/market_driver.cpp",
    "include/pineforge/native_host.hpp",
    "src/native_execution_consumer.hpp",
    "src/native_execution_consumer.cpp",
    "include/pineforge/native_order_identity.hpp",
    "include/pineforge/native_fx_curve.hpp",
    "src/native_fx_curve.cpp",
)

NATIVE_FX_CURVE_HEADER = "include/pineforge/native_fx_curve.hpp"
NATIVE_FX_CURVE_SOURCE = "src/native_fx_curve.cpp"
NATIVE_FX_CURVE_NAMESPACE = "native_fx_curve_v1"

DRIVER_FORWARD = (
    "inline namespace native_run_spec_v1 { struct NativeRunSpec; }"
)

TYPE_DEF = r'\b(?:enum\s+class|class|struct)\s+(\w+)\s*(?::[^;{]+)?\{'
ALIAS_DEF = r'\busing\s+(\w+)\s*='
METHOD_DEF = r'\b(\w+::[~\w]+)\s*\('
CALL_KEYWORDS = {
    "if", "for", "while", "switch", "catch", "return", "sizeof", "static_assert",
    "alignas", "noexcept", "decltype", "typeid", "throw", "new", "delete",
    "const_cast", "static_cast", "dynamic_cast", "reinterpret_cast",
    "requires", "offsetof", "defined", "case", "default", "sizeof",
}


def blank_compound_bodies(text):
    pattern = re.compile(TYPE_DEF)
    while True:
        match = pattern.search(text)
        if not match:
            return text
        start = match.end() - 1
        depth = 0
        for at in range(start, len(text)):
            depth += (text[at] == '{') - (text[at] == '}')
            if depth == 0:
                # Drop the whole compound so `class Foo {}` cannot rematch.
                text = text[:match.start()] + text[at + 1:]
                break
        else:
            raise ValueError("unclosed type body")


def namespace_functions(text):
    stripped = blank_compound_bodies(text)
    names = []
    for match in re.finditer(r'\b([A-Za-z_]\w*)\s*\(', stripped):
        name = match.group(1)
        if name in CALL_KEYWORDS or name.startswith("operator"):
            continue
        names.append(name)
    return names


def same_names(owner, value, pattern, label, version):
    owned = re.findall(pattern, owner)
    inner = re.findall(pattern, value)
    if owned != inner:
        raise ValueError(label + " must belong to " + version)
    return inner


def require(value, names, version, pattern):
    for name in names:
        if not re.search(pattern.replace("NAME", re.escape(name)), value):
            raise ValueError(name + " must belong to " + version)


def require_namespace_functions(value, names, version):
    found = namespace_functions(value)
    for name in names:
        if name not in found:
            raise ValueError(name + " must be a namespace-scope declaration in " + version)


def require_exact_alias(value, name, expected, version):
    aliases = list(re.finditer(
        r'\busing\s+' + re.escape(name) + r'\s*=\s*(.*?)\s*;', value, re.S))
    if len(aliases) != 1:
        raise ValueError(name + " requires exactly one alias in " + version)
    actual = re.sub(r'\s+', '', aliases[0].group(1))
    if actual != expected:
        raise ValueError(name + " must preserve its exact alias shape in " + version)


def versioned(text, outer, version):
    value = standalone_scope(text, outer, version)
    owner = body(clean(text), r'namespace\s+' + re.escape(outer) + r'\s*\{', outer)
    same_names(owner, value, TYPE_DEF, outer + " type definition", version)
    same_names(owner, value, ALIAS_DEF, outer + " type alias", version)
    same_names(owner, value, METHOD_DEF, outer + " out-of-line method", version)
    if namespace_functions(owner) != namespace_functions(value):
        raise ValueError(outer + " contains an unversioned free function")
    if (not re.search(TYPE_DEF, value) and not namespace_functions(value)
            and not re.findall(METHOD_DEF, value)):
        raise ValueError(version + " cannot be empty or comment-only")
    return value


def check_native_fx_curve(files):
    header = clean(files[NATIVE_FX_CURVE_HEADER])
    includes = re.findall(r'^\s*#\s*(include(?:_next)?)\b\s*([^\n]*)', header, re.M)
    if sorted((directive, target.strip()) for directive, target in includes) != [
            ('include', '<cstddef>'), ('include', '<cstdint>'), ('include', '<vector>')]:
        raise ValueError('native_fx_curve.hpp includes exactly cstddef, cstdint and vector')
    if re.search(r'\bNativeFxCurveSetupResult\b', header):
        raise ValueError('NativeFxCurveSetupResult belongs to the host surface, not the value header')
    curve = versioned(header, 'pineforge', NATIVE_FX_CURVE_NAMESPACE)
    same_names(header, curve, TYPE_DEF, 'FX-curve public type', NATIVE_FX_CURVE_NAMESPACE)
    same_names(header, curve, ALIAS_DEF, 'FX-curve public alias', NATIVE_FX_CURVE_NAMESPACE)
    if namespace_functions(header) != namespace_functions(curve):
        raise ValueError('FX-curve public functions must belong to native_fx_curve_v1')
    if re.findall(TYPE_DEF, curve) != [
            'NativeFxCurve', 'NativeFxCurveError', 'NativeFxCurveValidation']:
        raise ValueError('native_fx_curve_v1 requires exactly its three ordered value types')
    if re.findall(ALIAS_DEF, curve) or namespace_functions(curve) != [
            'validate_native_fx_curve', 'native_fx_curve_digest']:
        raise ValueError('native_fx_curve_v1 requires exactly its two ordered value functions')
    shapes = (
        (r'struct\s+NativeFxCurve\s*\{', 'NativeFxCurve',
         'std::vector<std::int64_t>effective_from_ms;'
         'std::vector<double>account_per_quote;'),
        (r'enum\s+class\s+NativeFxCurveError\s*:\s*std::uint8_t\s*\{',
         'NativeFxCurveError',
         'None=0,LengthMismatch=1,NotStrictlyIncreasing=2,'
         'NotFinitePositive=3,AllocationFailure=4,WrongPhase=5'),
        (r'struct\s+NativeFxCurveValidation\s*\{', 'NativeFxCurveValidation',
         'NativeFxCurveErrorerror=NativeFxCurveError::None;std::size_tindex=0;'),
    )
    for pattern, name, expected in shapes:
        shape = re.sub(r'\s+', '', body(curve, pattern, name))
        if shape.rstrip(',') != expected:
            raise ValueError(name + ' must preserve its native_fx_curve_v1 member order and shape')

    source = versioned(files[NATIVE_FX_CURVE_SOURCE], 'pineforge', NATIVE_FX_CURVE_NAMESPACE)
    functions = (
        ('NativeFxCurveValidation', 'validate_native_fx_curve'),
        ('std::uint64_t', 'native_fx_curve_digest'),
    )
    for return_type, name in functions:
        signature = (r'\b' + re.escape(return_type) + r'\s+' + name
                     + r'\s*\(\s*const\s+NativeFxCurve\s*&\s*(?:\w+\s*)?\)\s*noexcept\s*')
        for text, ending in ((curve, ';'), (blank_compound_bodies(source), r'\{')):
            matches = list(re.finditer(signature + ending, text))
            if len(matches) != 1:
                raise ValueError(name + ' requires exactly one native_fx_curve_v1 declaration/definition')
            prefix = text[:matches[0].start()]
            if prefix.count('{') != prefix.count('}'):
                raise ValueError(name + ' must be at native_fx_curve_v1 namespace scope')


def check_fx_curve_introduced_at(manifests):
    """The value header is new in its current owner, never part of an old host closure."""
    for label, manifest in manifests.items():
        if NATIVE_FX_CURVE_HEADER in manifest['files']:
            raise ValueError(NATIVE_FX_CURVE_HEADER + ' predates its introduction in '
                             + NATIVE_FX_CURVE_NAMESPACE + ': ' + label)


def authenticate_historical_host_manifests(root=ROOT, providers=PROVIDERS):
    """Reuse older authenticated closures without injecting current-only values.

    The frozen v15 provider is a same-epoch control, not evidence that a v15
    value predates its public owner.
    """
    manifests = {}
    with tempfile.TemporaryDirectory(prefix='.native-fx-introduced-', dir=root) as temporary:
        for label, provider in providers.items():
            if provider['engine_epoch'] == 'engine_script_run_v15':
                continue
            manifest_path = provider['manifest']
            if not manifest_path.parent.name.startswith('host-'):
                continue
            manifest_path = root / manifest_path.relative_to(ROOT)
            destination = Path(temporary) / label
            extract_tar((manifest_path.parent / provider['headers_name']).read_bytes(), destination)
            manifests[label] = authenticate_headers(
                destination, manifest_path, commit=provider['commit'], tree=provider['tree'])
    if not manifests:
        raise ValueError('native FX introduced-at check requires authenticated historical host closures')
    return manifests


def check_texts(files):
    check_native_fx_curve(files)
    identity = versioned(files[FILES[11]], "pineforge::native_order", "native_order_v1")
    require(identity, ("RunIdentity", "RequestHandle", "Birth"),
            "native_order_v1", r'\b(?:class|struct)\s+NAME\s*\{')
    order = versioned(files[FILES[0]], "pineforge::native_order", "native_order_v4")
    require(order, ("WorkingRequestCore", "Request", "SubmitResult",
                    "AcceptedEvent", "NoEffectEvent", "MatchRejectedEvent",
                    "ExecutionAppliedEvent", "HostSized", "HostSizedKind", "ReverseTo",
                    "RemainingDeferred", "RemainingProjectionDeferred", "AllowanceDeferred",
                    "OpeningShape", "ExecutionTerms", "TermsResolvedInput",
                    "TermsResolvedEvent", "NativeCandidatePriceKind"),
            "native_order_v4", r'\b(?:enum\s+class|class|struct)\s+NAME\s*(?::[^;{]+)?\{')
    require(order, ("CommandEvent", "ExecutionPlan", "OrderIntent", "Remaining",
                    "RemainingProjection", "Allowance"),
            "native_order_v4", r'\busing\s+NAME\s*=')
    require_exact_alias(
        order, "OrderIntent", "std::variant<Flatten,Reduce,Transact,ReverseTo,HostSized>",
        "native_order_v4")
    require_exact_alias(
        order, "Remaining",
        "std::variant<RemainingUnbound,RemainingFlattenAll,RemainingUnits,RemainingDeferred>",
        "native_order_v4")
    require_exact_alias(
        order, "RemainingProjection",
        "std::variant<RemainingProjectionUnbound,RemainingProjectionFlattenAll,"
        "RemainingProjectionUnits,RemainingProjectionDeferred>", "native_order_v4")
    require_exact_alias(
        order, "Allowance",
        "std::variant<AllowanceUnset,AllowanceUnits,AllowanceAllScope,AllowanceDeferred>",
        "native_order_v4")
    require_exact_alias(
        order, "ExecutionPlan",
        "std::variant<execution::Flatten,order_action::Reduce,order_action::Transact,"
        "execution::ReverseTo>", "native_order_v4")
    require_namespace_functions(order, ("to_execution_plan",), "native_order_v4")
    required_order_members = (
        (r'\bPreparation<PreparedMutation>\s+prepare_terms\s*\(', "prepare_terms"),
        (r'\bstatic\s+Allowance\s+evaluated_allowance\s*\(', "evaluated_allowance"),
        (r'\bstatic\s+bool\s+effective_host_units\s*\(', "effective_host_units"),
    )
    for pattern, name in required_order_members:
        if len(re.findall(pattern, order)) != 1:
            raise ValueError(name + " must be a native_order_v4 WorkingRequestCore member")
    if re.search(r'\b(?:class|struct)\s+RunIdentity\s*\{', order):
        raise ValueError("RunIdentity must remain in native_order_v1, not native_order_v4")
    order_src = versioned(files[FILES[1]], "pineforge::native_order", "native_order_v4")
    require(order_src, ("WorkingRequestCore::reset", "WorkingRequestCore::find_live",
                        "WorkingRequestCore::prepare_terms",
                        "WorkingRequestCore::evaluated_allowance",
                        "WorkingRequestCore::effective_host_units"),
            "native_order_v4", r'\bNAME\s*\(')

    calendar = versioned(files[FILES[2]], "pineforge::native_calendar", "native_calendar_v2")
    require(calendar, ("Timeframe", "SessionCalendar", "NativeInterval",
                       "TimezoneIdentityDescriptor"),
            "native_calendar_v2", r'\b(?:class|struct)\s+NAME\s*\{')
    require_namespace_functions(
        calendar,
        ("parse_timeframe", "parse_session", "period_key", "interval_containing",
         "timezone_identity_descriptor", "timezone_accepted"),
        "native_calendar_v2")
    calendar_src = versioned(files[FILES[3]], "pineforge::native_calendar", "native_calendar_v2")
    require_namespace_functions(
        calendar_src,
        ("parse_timeframe", "parse_session", "period_key", "interval_containing",
         "timezone_identity_descriptor"),
        "native_calendar_v2")
    require(calendar_src, ("TimezoneIdentityDescriptor::valid",),
            "native_calendar_v2", r'\bNAME\s*\(')

    spec = versioned(files[FILES[4]], "pineforge", "native_run_spec_v1")
    require(spec, ("NativeRunSpec", "NativeRunSpecValidation", "NativeRunSpecError",
                   "NativeRunSpecField"),
            "native_run_spec_v1",
            r'\b(?:enum\s+class|struct)\s+NAME\s*(?::[^;{]+)?\{')
    require_namespace_functions(
        spec, ("validate_native_run_spec", "normalize_native_run_spec"),
        "native_run_spec_v1")
    spec_src = versioned(files[FILES[5]], "pineforge", "native_run_spec_v1")
    require_namespace_functions(
        spec_src, ("validate_native_run_spec", "normalize_native_run_spec"),
        "native_run_spec_v1")

    driver_text = files[FILES[6]]
    if driver_text.count(DRIVER_FORWARD) != 1:
        raise ValueError("market_driver.hpp must forward-declare NativeRunSpec "
                         "in native_run_spec_v1 outside native_driver_v4")
    driver_clean = clean(driver_text)
    driver_owner = body(driver_clean, r'namespace\s+pineforge\s*\{', "pineforge")
    driver = body(driver_owner, r'inline\s+namespace\s+native_driver_v4\s*\{',
                  "native_driver_v4")
    if DRIVER_FORWARD in driver or "native_run_spec_v1" in driver:
        raise ValueError("NativeRunSpec forward declaration must stay outside native_driver_v4")
    if re.search(r'\bstruct\s+NativeRunSpec\s*\{', driver_clean):
        raise ValueError("NativeRunSpec definition does not belong to native_driver_v4")
    # Compare ownership as if the allowed forward declaration were absent.
    driver_without_forward = driver_clean.replace(DRIVER_FORWARD, "", 1)
    versioned(driver_without_forward, "pineforge", "native_driver_v4")
    require(driver, ("NativeCoordinate", "NativeDriverPoint", "NativeDecisionContext",
                     "NativeInputPreflightResult", "INativeDriverSink"),
            "native_driver_v4", r'\b(?:class|struct)\s+NAME\s*\{')
    require_namespace_functions(
        driver, ("native_bar_structurally_valid", "preflight_native_inputs"),
        "native_driver_v4")
    if 'kNativeConsumerSemanticVersion = "native-consumer/v6"' not in driver_text:
        raise ValueError("consumer semantic marker must remain native-consumer/v6")
    provenance = body(driver, r'enum\s+class\s+NativePriceProvenance\s*:[^{]+\{', 'price provenance')
    expected_provenance = [('Confirmed', '0'), ('ObservedPrint', '1'), ('ModeledOHLCOpen', '2'),
        ('ModeledOHLCClose', '3'), ('CarriedOpen', '4'), ('AfterCalculationClose', '5'),
        ('PartialFinalized', '6'), ('Calculation', '7'), ('CurrentExecution', '8')]
    if re.findall(r'(\w+)\s*=\s*(\d+)', provenance) != expected_provenance:
        raise ValueError('driver provenance must preserve 0..7 and append only CurrentExecution=8')
    if 'kNativeDriverSemanticVersion = "native-driver/v4"' not in driver_text:
        raise ValueError('driver semantic marker must be native-driver/v4')
    driver_src = versioned(files[FILES[7]], "pineforge", "native_driver_v4")
    require_namespace_functions(
        driver_src, ("native_bar_structurally_valid", "preflight_native_inputs"),
        "native_driver_v4")

    host = versioned(files[FILES[8]], "pineforge", "engine_script_run_v16")
    require(host, ("NativeStrategyHost", "NativeStateView", "NativeLifecycleKind",
                   "NativeFailure", "NativeFailureContext", "NativeInRunCause",
                   "NativeInRunRecipient", "NativeInRunCursor", "NativeMarketEvent",
                   "NativeSetupResult", "NativePhysicalPosition", "NativeAccountObservation",
                   "NativeCurrentPriceRule", "NativeCurrentQuoteKind", "NativeCurrentPointView",
                   "NativeCurrentRefusal", "NativeCurrentExecution", "NativeCurrentExecutionPreview",
                   "NativeExecutionTermsFacts", "NativePrecommitView",
                   "NativePrecommitVerdict", "NativeFxCurveSetupResult"),
            "engine_script_run_v16",
            r'\b(?:enum\s+class|class|struct)\s+NAME\s*(?::[^;{]+)?\{')
    require(host, ("NativeCurrentExecutionResult",), "engine_script_run_v16",
            r'\busing\s+NAME\s*=')
    require_exact_alias(
        host, "NativeCurrentExecutionResult",
        "std::variant<NativeCurrentRefusal,native_order::ExecutionAppliedEvent,"
        "native_order::NoEffectEvent,native_order::MatchRejectedEvent,"
        "native_order::CancelledEvent>", "engine_script_run_v16")
    current_command = body(host, r'struct\s+NativeCurrentExecution\s*\{', 'current command')
    if re.sub(r'\s+', '', current_command) != 'native_order::RequestHandletarget;NativeCurrentPriceRuleprice_rule=NativeCurrentPriceRule::AsPresented;':
        raise ValueError('current command has exactly target and price_rule, no competing selected authority')
    preview = body(host, r'struct\s+NativeCurrentExecutionPreview\s*\{', 'current preview')
    compact_preview = re.sub(r'\s+', '', preview)
    expected_preview = ('std::optional<NativeCurrentRefusal>refusal;'
        'std::optional<execution::Status>settlement_readiness;'
        'execution::AccountEffectProjectionaccount;std::vector<double>closed_row_pnl;'
        'std::optional<native_order::MatchRejectReason>terms_rejection;'
        'std::optional<native_order::CancelReason>terms_cancellation;')
    if compact_preview != expected_preview:
        raise ValueError('preview must preserve independent readiness immediately after refusal')
    required_host_methods = (
        (r'\bvirtual\s+native_order::ExecutionTerms\s+resolve_execution_terms\s*\('
         r'\s*const\s+NativeExecutionTermsFacts\s*&', "resolve_execution_terms"),
        (r'\bvirtual\s+NativePrecommitVerdict\s+validate_execution_precommit\s*\('
         r'\s*const\s+NativePrecommitView\s*&', "validate_execution_precommit"),
        (r'\bNativeFxCurveSetupResult\s+configure_native_fx_curve\s*\('
         r'\s*const\s+NativeFxCurve\s*&', "configure_native_fx_curve"),
    )
    for pattern, name in required_host_methods:
        if len(re.findall(pattern, host)) != 1:
            raise ValueError(name + " must be a v16 NativeStrategyHost member")
    for name in ('on_native_applied', 'current_execution_point', 'inspect_current_execution', 'execute_current'):
        if name not in host:
            raise ValueError('missing current host contract: ' + name)
    if "native_failure_context_in_run" not in host:
        raise ValueError("native_failure_context_in_run must belong to engine_script_run_v16")
    if "native_failed_run_identity" not in host:
        raise ValueError("native_failed_run_identity must belong to engine_script_run_v16")
    if not re.search(r'\bSubmitResult\s+submit\s*\(\s*const\s+native_order::Request\s*&', host):
        raise ValueError("general submit must belong to engine_script_run_v16")
    if not re.search(r'\bReplaceResult\s+replace\s*\(\s*const\s+native_order::RequestHandle\s*&',
                     host):
        raise ValueError("general replace must belong to engine_script_run_v16")
    if "submit_market" not in host or "replace_market" not in host:
        raise ValueError("market-only submit/replace must remain in engine_script_run_v16")
    consumer = versioned(files[FILES[9]], "pineforge", "engine_script_run_v16")
    require(consumer, ("NativeExecutionConsumer",),
            "engine_script_run_v16", r'\bclass\s+NAME\s*')
    consumer_src = versioned(files[FILES[10]], "pineforge", "engine_script_run_v16")
    require(consumer_src,
            ("NativeStrategyHost::configure_native", "NativeStrategyHost::native_state",
             "NativeStrategyHost::native_events",
             "NativeStrategyHost::configure_native_fx_curve"),
            "engine_script_run_v16", r'\bNAME\s*\(')


    # These are continuation owners, not redundant physical-book snapshots.
    hash_requirements = {
        'hash_owner': ('native_order::BindOpenings', 'bind->cycle', 'bind->openings.size()', 'hash_handle(f, handle)'),
        'hash_authority': ('native_order::OpeningsClose', 'openings->cycle', 'openings->side',
                           'openings->openings.size()', 'openings->enrollment.index()', 'hash_handle(f, handle)'),
        'hash_scope': ('native_order::SelectedExposure', 'selected->cycle',
                       'selected->incarnations.size()', 'f.u(incarnation)'),
        'hash_current_point': ('point.decision.coordinate', 'point.decision.decision_floor_ms',
                              'point.decision.input_interval', 'point.decision.script_interval',
                              'point.price', 'point.quote_kind', 'point.quote_origin_ordinal'),
    }
    for function, facts in hash_requirements.items():
        fold = body(consumer_src, r'void\s+' + function + r'\s*\([^)]*\)\s*noexcept\s*\{', function)
        for fact in facts:
            if fact not in fold:
                raise ValueError(function + ' omits native continuation fact: ' + fact)
    continuation = body(consumer_src, r'uint64_t\s+NativeExecutionConsumer::continuation_hash\(\)\s*const\s*noexcept\s*\{', 'native continuation')
    for fact in ('current_frame_.has_value()', 'current_frame_->point', 'current_frame_->acceptance_cutoff',
                 'applied_notifications_.size() - notification_head_', 'notification.history_index',
                 'notification.ordinal', 'notification.point', 'consuming_request_', 'draining_notifications_'):
        if fact not in continuation:
            raise ValueError('native continuation omits current frame/queue fact: ' + fact)


def load(root=ROOT):
    return {name: (root / name).read_text() for name in FILES}


def check(root=ROOT):
    check_texts(load(root))
    check_fx_curve_introduced_at(authenticate_historical_host_manifests(root))


if __name__ == "__main__":
    check()
    print("native_order identity v1 / values v4, native_calendar_v2, native_run_spec_v1, "
          "native_driver_v4, native_fx_curve_v1 and host engine_script_run_v16 ownership verified")
