#!/usr/bin/env python3
"""Source-only aggregate/standalone ABI ownership guard; no runtime execution."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
FILES = (
    "include/pineforge/engine.hpp", "src/engine_state_hash.cpp", "src/engine_stream.cpp",
    "include/pineforge/exit_leg_lifecycle.hpp", "include/pineforge/market_admission.hpp",
    "src/market_admission.cpp", "include/pineforge/reservation_expansion.hpp",
    "src/reservation_expansion.cpp", "include/pineforge/order_cancellation.hpp",
    "include/pineforge/source/pine_adapter.hpp", "include/pineforge/source/pine_pending_intent.hpp",
    "src/source/pine_state_hash.cpp",
)


def clean(text):
    return re.sub(r'//[^\n]*|/\*.*?\*/', '', text, flags=re.S)


def body(text, pattern, name):
    matches = list(re.finditer(pattern, text))
    if len(matches) != 1:
        raise ValueError(name + " requires exactly one definition")
    start = matches[0].end()
    depth = 1
    for at in range(start, len(text)):
        depth += (text[at] == '{') - (text[at] == '}')
        if depth == 0:
            return text[start:at]
    raise ValueError(name + " has an unclosed body")


def standalone_scope(text, outer, version):
    text = clean(text)
    owner = body(text, r'namespace\s+' + re.escape(outer) + r'\s*\{', outer)
    value = body(owner, r'inline\s+namespace\s+' + version + r'\s*\{', version)
    # Every defined type, including nested helper types, must be inside the
    # versioned owner; a comment or empty namespace cannot satisfy this guard.
    types = r'\b(?:enum\s+class|class|struct)\s+(\w+)\s*(?::[^;{]+)?\{'
    if re.findall(types, owner) != re.findall(types, value):
        raise ValueError(outer + " contains an unversioned type definition")
    return value


def check_texts(files):
    header = clean(files[FILES[0]])
    namespaces = re.findall(r'inline\s+namespace\s+(engine_script_run_v\d+)\s*\{', header)
    if namespaces != ["engine_script_run_v16"]:
        raise ValueError("BacktestEngine requires engine_script_run_v16")
    pending_forward = re.findall(
        r'namespace\s+source\s*\{\s*struct\s+PendingOrder\s*;\s*'
        r'struct\s+StrategyOverrides\s*;\s*}', header)
    if len(pending_forward) != 1 or re.search(r'\busing\s+PendingOrder\b', header):
        raise ValueError("engine header must only forward-declare source::PendingOrder")
    broker = body(clean(files[FILES[1]]),
                  r'uint64_t\s+BacktestEngine::broker_state_hash\(\)\s+const\s*\{', "broker hash")
    if not re.match(r'\s*BrokerStateHashSink\s+f;\s*f\.s\("pineforge-broker-state/v16"\);', broker):
        raise ValueError("broker entry requires v16 domain")
    if 'kSourceAdapterDomain[] = "pineforge-source-adapter/v1"' not in files[FILES[9]]:
        raise ValueError("source adapter domain must remain in pine_adapter.hpp")
    source_hash = body(clean(files[FILES[11]]),
                       r'void\s+source::PineStrategyHost::hash_source_extension\(BrokerStateHashSink&\s+f\)\s+const\s*\{',
                       "source hash")
    if not re.match(r'\s*f\.s\(kSourceAdapterDomain\);', source_hash):
        raise ValueError("source hash must begin with its adapter domain")
    stream = body(clean(files[FILES[2]]),
                  r'uint64_t\s+BacktestEngine::stream_state_hash\(\)\s+const\s*\{', "stream hash")
    compact = re.sub(r'\s+', '', stream)
    fold = "integer(16);integer(broker_state_hash());"
    if compact.count(fold) != 1:
        raise ValueError("stream entry requires v16 then broker hash")
    prefix = compact[:compact.index(fold)]
    if prefix.count('{') != prefix.count('}') or (prefix and prefix[-1] not in ';}'):
        raise ValueError("stream v16 fold must be unconditional")
    life = standalone_scope(files[FILES[3]], "pineforge::exit_legs", "lifecycle_v1")
    for name in ("Lifecycle", "Definition", "Action", "Frame", "Barrier", "Suspension"):
        if not re.search(r'\b(?:class|struct)\s+' + name + r'\s*\{', life):
            raise ValueError(name + " must belong to lifecycle_v1")
    admission = standalone_scope(files[FILES[4]], "pineforge::admission", "market_admission_v2")
    for name in ("Draft", "Journal", "Allocation", "CommandCapture", "ReviewCapture", "CommandObservation",
                 "CommandEvent", "ReviewEvent", "SizingEvent", "Field"):
        if not re.search(r'\b(?:class|struct)\s+' + name + r'\s*\{', admission):
            raise ValueError(name + " must belong to market_admission_v2")
    cancellation = standalone_scope(files[FILES[8]], "pineforge", "order_cancellation_v1")
    for name in ("CancellationCause", "CancellationState", "CloseClaimRelease",
                 "CancellationResult", "CancellationTarget", "OrderCancellationReceipt"):
        if not re.search(r'\b(?:enum\s+class|class|struct)\s+' + name
                         + r'\s*(?::[^;{]+)?\{', cancellation):
            raise ValueError(name + " must belong to order_cancellation_v1")
    source = clean(files[FILES[5]])
    implementation = standalone_scope(source, "pineforge::admission", "market_admission_v2")
    methods = r'\b(?:Draft|Journal|Allocation|CommandCapture|ReviewCapture)::[~\w]+\s*\('
    if not re.findall(methods, source) or re.findall(methods, source) != re.findall(methods, implementation):
        raise ValueError("admission out-of-line methods need their versioned owner")
    for name in FILES[6:8]:
        text = clean(files[name])
        if len(re.findall(r'inline\s+namespace\s+reservation_expansion_v1\s*\{', text)) != 1:
            raise ValueError("unchanged reservation ABI must remain v1")
    for name, text in files.items():
        if name.startswith("include/pineforge/compat/pine/"):
            found = re.findall(r'inline\s+namespace\s+(engine_script_run_v\d+)\s*\{', clean(text))
            if found:
                raise ValueError(name + " must forward-declare source::PendingOrder without an engine namespace")


def load(root=ROOT):
    result = {name: (root / name).read_text() for name in FILES}
    for path in (root / "include/pineforge/compat/pine").glob("*.hpp"):
        result[str(path.relative_to(root))] = path.read_text()
    return result


def check(root=ROOT):
    check_texts(load(root))


if __name__ == "__main__":
    check()
    print("aggregate v16/source-adapter, standalone admission v2 and lifecycle/cancellation v1 ownership verified")
