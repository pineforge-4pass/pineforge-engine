#!/usr/bin/env python3
"""Source-only native C++ ABI ownership guard; no compiler or engine runs."""
from pathlib import Path
import re

from check_aggregate_cpp_versions import body, clean, standalone_scope

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
)

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


def check_texts(files):
    identity = versioned(files[FILES[11]], "pineforge::native_order", "native_order_v1")
    require(identity, ("RunIdentity", "RequestHandle", "Birth"),
            "native_order_v1", r'\b(?:class|struct)\s+NAME\s*\{')
    order = versioned(files[FILES[0]], "pineforge::native_order", "native_order_v2")
    require(order, ("WorkingRequestCore", "Request", "SubmitResult",
                    "AcceptedEvent", "NoEffectEvent", "MatchRejectedEvent",
                    "ExecutionAppliedEvent"),
            "native_order_v2", r'\b(?:class|struct)\s+NAME\s*\{')
    if "CommandEvent" not in re.findall(ALIAS_DEF, order):
        raise ValueError("CommandEvent must belong to native_order_v2")
    if re.search(r'\b(?:class|struct)\s+RunIdentity\s*\{', order):
        raise ValueError("RunIdentity must remain in native_order_v1, not native_order_v2")
    order_src = versioned(files[FILES[1]], "pineforge::native_order", "native_order_v2")
    require(order_src, ("WorkingRequestCore::reset", "WorkingRequestCore::find_live"),
            "native_order_v2", r'\bNAME\s*\(')

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
                         "in native_run_spec_v1 outside native_driver_v3")
    driver_clean = clean(driver_text)
    driver_owner = body(driver_clean, r'namespace\s+pineforge\s*\{', "pineforge")
    driver = body(driver_owner, r'inline\s+namespace\s+native_driver_v3\s*\{',
                  "native_driver_v3")
    if DRIVER_FORWARD in driver or "native_run_spec_v1" in driver:
        raise ValueError("NativeRunSpec forward declaration must stay outside native_driver_v3")
    if re.search(r'\bstruct\s+NativeRunSpec\s*\{', driver_clean):
        raise ValueError("NativeRunSpec definition does not belong to native_driver_v3")
    # Compare ownership as if the allowed forward declaration were absent.
    driver_without_forward = driver_clean.replace(DRIVER_FORWARD, "", 1)
    versioned(driver_without_forward, "pineforge", "native_driver_v3")
    require(driver, ("NativeCoordinate", "NativeDriverPoint", "NativeDecisionContext",
                     "NativeInputPreflightResult", "INativeDriverSink"),
            "native_driver_v3", r'\b(?:class|struct)\s+NAME\s*\{')
    require_namespace_functions(
        driver, ("native_bar_structurally_valid", "preflight_native_inputs"),
        "native_driver_v3")
    if 'kNativeConsumerSemanticVersion = "native-consumer/v4"' not in driver_text:
        raise ValueError("consumer semantic marker must remain native-consumer/v4")
    driver_src = versioned(files[FILES[7]], "pineforge", "native_driver_v3")
    require_namespace_functions(
        driver_src, ("native_bar_structurally_valid", "preflight_native_inputs"),
        "native_driver_v3")

    host = versioned(files[FILES[8]], "pineforge", "engine_script_run_v13")
    require(host, ("NativeStrategyHost", "NativeStateView", "NativeLifecycleKind",
                   "NativeFailure", "NativeFailureContext", "NativeInRunCause",
                   "NativeInRunRecipient", "NativeInRunCursor", "NativeMarketEvent",
                   "NativeSetupResult", "NativePhysicalPosition", "NativeAccountObservation"),
            "engine_script_run_v13",
            r'\b(?:enum\s+class|class|struct)\s+NAME\s*(?::[^;{]+)?\{')
    if "native_failure_context_in_run" not in host:
        raise ValueError("native_failure_context_in_run must belong to engine_script_run_v13")
    if "native_failed_run_identity" not in host:
        raise ValueError("native_failed_run_identity must belong to engine_script_run_v13")
    if not re.search(r'\bSubmitResult\s+submit\s*\(\s*const\s+native_order::Request\s*&', host):
        raise ValueError("general submit must belong to engine_script_run_v13")
    if not re.search(r'\bReplaceResult\s+replace\s*\(\s*const\s+native_order::RequestHandle\s*&',
                     host):
        raise ValueError("general replace must belong to engine_script_run_v13")
    if "submit_market" not in host or "replace_market" not in host:
        raise ValueError("market-only submit/replace must remain in engine_script_run_v13")
    consumer = versioned(files[FILES[9]], "pineforge", "engine_script_run_v13")
    require(consumer, ("NativeExecutionConsumer",),
            "engine_script_run_v13", r'\bclass\s+NAME\s*')
    consumer_src = versioned(files[FILES[10]], "pineforge", "engine_script_run_v13")
    require(consumer_src,
            ("NativeStrategyHost::configure_native", "NativeStrategyHost::native_state",
             "NativeStrategyHost::native_events"),
            "engine_script_run_v13", r'\bNAME\s*\(')


def load(root=ROOT):
    return {name: (root / name).read_text() for name in FILES}


def check(root=ROOT):
    check_texts(load(root))


if __name__ == "__main__":
    check()
    print("native_order identity v1 / values v2, native_calendar_v2, native_run_spec_v1, "
          "native_driver_v3 and host engine_script_run_v13 ownership verified")
