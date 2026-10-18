#!/usr/bin/env python3
"""Source-only native C++ ABI ownership guard; no compiler or engine runs."""
from pathlib import Path
import re
import sys
import tempfile

from check_aggregate_cpp_versions import body, clean, standalone_scope
from prepare_settlement_cpp_abi_base import PROVIDERS, authenticate_headers, extract_tar

ROOT = Path(__file__).resolve().parents[1]
# The private-fixture PRs move tests/fixtures/native_cpp_abi/ out of the
# public tree, where authenticate_historical_host_manifests() reads one
# headers.tar per host closure.  Absent, the guard prints the project's pinned
# literal -- the one the private-fixture ctest rows print and
# tests/CMakeLists.txt matches with SKIP_REGULAR_EXPRESSION; ci_verify and
# ci_preflight read a source guard's 77 plus that literal as a named skip --
# and exits 77 rather than failing on read_bytes().  The skip is PARTIAL: the
# text half (check_texts) reads only public sources and runs first, so it
# enforces the same ownership checks in the fixture-absent tree it always
# enforced, and its failure is that failure -- exit 1, no skip. Only the
# historical-manifest half is skipped. Present, every check below runs as
# before (R2-F4).
PRIVATE_FIXTURE_SKIP_LITERAL = "SKIP private fixtures not present: "
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
    'include/pineforge/native_bar_views.hpp',
    'src/native_bar_views.cpp',
    'src/source/pine_security_eval.cpp',
    'src/source/pine_strategy_host.cpp',
    'src/source/pine_adapter.cpp',
    'src/source/pine_scheduler_native.cpp',
    'src/source/magnifier_intrabars.cpp',
)

NATIVE_FX_CURVE_HEADER = "include/pineforge/native_fx_curve.hpp"
NATIVE_FX_CURVE_SOURCE = "src/native_fx_curve.cpp"
NATIVE_FX_CURVE_NAMESPACE = "native_fx_curve_v1"

# CR-150: approved v1 sibling bridges, as narrow as the original v3 digest
# exception. Every other definition stays owned by its existing epoch.
NATIVE_BAR_VIEWS_HEADER = 'include/pineforge/native_bar_views.hpp'
NATIVE_BAR_VIEWS_SOURCE = 'src/native_bar_views.cpp'
NATIVE_BAR_VIEWS_NAMESPACE = 'native_bar_views_v1'
SPEC_VIEWS_BRIDGE = """inline namespace native_bar_views_v1 {

NativeRunSpecValidation normalize_native_run_spec(
        NativeRunSpec& spec, const NativeBarViews& views) noexcept {
    const auto judged = native_run_spec_v3::validate_run_spec_views(spec, views);
    if (!judged) return judged;
    if (spec.fee_value == 0.0) spec.fee_value = 0.0;
    return {};
}

NativeRunSpecValidation validate_native_instrument_feeds(
        const std::vector<NativeInstrumentFeed>& feeds, const NativeBarViews& views) noexcept {
    try {
        return native_run_spec_v3::instrument_feed_shapes(feeds, views);
    } catch (const std::bad_alloc&) {
        return {NativeRunSpecError::AllocationFailure, NativeRunSpecField::InstrumentFeedTimeframe};
    } catch (...) {
        return {NativeRunSpecError::CalendarFailure, NativeRunSpecField::InstrumentFeedTimeframe};
    }
}

NativeRunSpecValidation validate_native_timeframe_subscriptions(
        const std::vector<NativeTimeframeSubscription>& subscriptions,
        const std::string& input_tf, bool timeframe_undetected,
        const std::optional<NativeAuxiliaryFeed>& auxiliary_feed,
        const std::vector<NativeInstrumentFeed>& feeds, const NativeBarViews& views) noexcept {
    return native_run_spec_v3::validate_subscription_views(
        subscriptions, input_tf, timeframe_undetected, auxiliary_feed, feeds, views);
}

std::uint64_t native_instrument_feeds_digest(
        const std::vector<NativeInstrumentFeed>& feeds, const NativeBarViews& views) noexcept {
    return native_run_spec_v3::digest_instrument_feed_views(feeds, views);
}

} // inline namespace native_bar_views_v1
"""
CONSUMER_VIEWS_BRIDGE = """inline namespace native_bar_views_v1 {

std::uint64_t native_run_spec_digest(const NativeRunSpec& spec, const NativeBarViews& views) {
    NativeExecutionConsumer::SpecBarDigests digests;
    digests.intrabar = native_bar_views_v1::native_intrabar_path_digest(spec.intrabar, views.lower_path);
    if (!spec.subscriptions.empty())
        digests.subscriptions = native_timeframe_subscriptions_digest(spec.subscriptions);
    if (spec.auxiliary_feed)
        digests.auxiliary = native_auxiliary_feed_digest(*spec.auxiliary_feed);
    if (!spec.instrument_feeds.empty())
        digests.instruments = native_bar_views_v1::native_instrument_feeds_digest(spec.instrument_feeds, views);
    Fnv f;
    f.run_base = spec.identity.run_number;
    hash_spec(f, spec, &digests);
    return f.h;
}

} // inline namespace native_bar_views_v1
"""

# Every .bars/.close_ms and ->bars/->close_ms access in the named read set.
# Storage writes are included deliberately. L15b/L16 update their rows when
# introducing a caller/provider; an unlisted access fails before ABI fixtures.
BAR_VIEW_READS = {
    'src/native_bar_views.cpp': (
        'return {borrowed->bars, borrowed->close_ms, borrowed->bars.size};',
        'return {{owned.bars.data(), owned.bars.size()},',
        'owned.close_ms.data(), owned.close_ms.size()};',
        'if (lower == nullptr || !lower->bars.empty())',
    ),
    'src/native_run_spec.cpp': (
        'const auto bars = data.bars;',
        'if ((borrowed != nullptr && !feed.bars.empty())',
        'if ((borrowed != nullptr && !feed.close_ms.empty())',
        '|| (data.close_count != 0 && data.close_ms == nullptr))',
        'if (data.close_ms[i] <= bars.data[i].timestamp',
        '|| (i + 1 < bars.size && data.close_ms[i] > bars.data[i + 1].timestamp)) {',
        'for (std::size_t i = 0; i < feed->bars.size(); ++i) {',
        'if (!native_bar_structurally_valid(feed->bars[i])) {',
        'if (i > 0 && feed->bars[i].timestamp <= feed->bars[i - 1].timestamp) {',
        '&& (spec.intrabar.lower() == nullptr || !spec.intrabar.lower()->bars.empty()))',
        'u(lower->bars.size());',
        'for (const auto& bar : lower->bars) {',
        'u(feed.bars.size());',
        'for (const auto& bar : feed.bars) {',
        'u(data.bars.size);',
        'for (std::size_t k = 0; k < data.bars.size; ++k) {',
        'const Bar& bar = data.bars.data[k];',
        'i(k < data.close_count ? data.close_ms[k] : 0);',
    ),
    'src/native_execution_consumer.hpp': (
    ),
    'src/native_execution_consumer.cpp': (
        'begin_bars_ = args.bars;',
        'descriptor->bars.data = nullptr;',
        'descriptor->close_ms = nullptr;',
        'if (feed.seconds == kSecPerDay && !feed.bars.empty()) {',
        'daily = &feed.bars;',
        'const auto count = source ? active_bar_views_.lower_count : lower.bars.size();',
        'path_spec, lower.bars.empty() ? nullptr : lower.bars.data(),',
        'static_cast<int>(lower.bars.size()), NativeInputPolicy::Batch);',
        'lower->bars.begin(), lower->bars.end(), begin,',
        'for (; candidate != lower->bars.end() && candidate->timestamp < end; ++candidate) {',
        'spec && spec->auxiliary_feed ? spec->auxiliary_feed->bars.size() : 0;',
        'const auto& declared = spec_ptr()->auxiliary_feed->bars;',
        'const std::size_t total = feed.bars.size;',
        '? feed.bars.data[at].timestamp <= horizon.open_ms',
        ': feed.close_ms[at] <= horizon.last_traded_close_ms;',
        'subscription.latest = feed.bars.data[at];',
        'context.interval.open_ms = feed.bars.data[at].timestamp;',
        'context.interval.eligible_open_ms = feed.bars.data[at].timestamp;',
        'context.interval.last_traded_close_ms = feed.close_ms[at];',
        'context.interval.next_period_open_ms = feed.close_ms[at];',
        'at + 1 < total ? feed.bars.data[at + 1].timestamp : feed.close_ms[at];',
        'if (!invoke_timeframe_callback(engine, feed.bars.data[at], context)) return false;',
    ),
    'src/source/pine_security_eval.cpp': (
        'feed.bars = held.bars;',
        'feed.close_ms = held.close_ms;',
        'site.close_ms = 0;',
        'site.close_ms = close_ms;',
        'while (static_cast<std::size_t>(site.delivered) < feed.bars.size) {',
        'const bool visible = site.lookahead ? feed.bars.data[at].timestamp <= chart_bar.timestamp',
        ': feed.close_ms[at] <= chart_close;',
        'run_foreign_site_bar(site, feed.bars.data[at], feed.close_ms[at]);',
        'feed.bars.assign(bars, bars + n);',
        'feed.close_ms.assign(close_ms, close_ms + n);',
        'if (n < 0 || static_cast<std::size_t>(n) != data.bars.size || (n > 0 && values == nullptr))',
        '+ " values for " + std::to_string(data.bars.size) + " bars");',
        'f.u(native_instrument_feed_view(feed, nullptr).bars.size);',
    ),
    'src/source/pine_strategy_host.cpp': (
        'if (args.n > 0 && args.bars == nullptr) {',
        'const Bar& bar = args.bars[i];',
        'const std::int64_t previous = args.bars[i - 1].timestamp;',
        '&& (!std::isfinite(args.bars[args.n - 1].close)',
        '|| args.bars[args.n - 1].close <= 0.0)) {',
        'lent[i].bars.swap(store[i].bars);',
        'lent[i].close_ms.swap(store[i].close_ms);',
        'store[i].bars.swap(lent[i].bars);',
        'store[i].close_ms.swap(lent[i].close_ms);',
        '{feed.instrument, feed.tf, data.bars.size, feed.columns.size()});',
        'if (effective_input.empty() && args.n >= 2 && args.bars != nullptr)',
        'effective_input = detect_timeframe(args.bars, args.n);',
    ),
    'src/source/pine_adapter.cpp': (
        'if (effective_input.empty() && args.n >= 2 && args.bars != nullptr) {',
        'effective_input = detect_timeframe(args.bars, args.n);',
        'make_magnifier_path_source(args.bars, args.n, spec.input_tf,',
        '} else if (args.bars && args.n > 0) {',
        'path.bars = tradingview_magnifier_bars(',
        'args.bars, args.n, spec.input_tf, spec.script_tf, spec.session,',
        'const bool inactive_sampler_path = inert != nullptr && inert->bars.empty()',
    ),
    'src/source/pine_scheduler_native.cpp': (
        'if (args.bars && args.n > 0) {',
        'if (args.is_stream) next.owned.assign(args.bars, args.bars + args.n);',
        'next.bars = InputView(args.is_stream ? next.owned.data() : args.bars,',
        'retained_n_ = retained_.bars.size();',
        'retained_.bars = {};',
        'input_script_completes_.assign(retained_.bars.size(), 1U);',
        'input_script_boundary_completes_.assign(retained_.bars.size(), 0U);',
        'retained_.bars.data(), retained_.bars.size(), input_script_completes_,',
        'selected_executed_source_bars(host, state.spec, window, retained_.bars);',
        'host.scheduler_prepare_script_run(retained_.bars, static_eligible,',
        'host.scheduler_prepare_security_sequence(retained_.bars);',
        'lower->bars.begin(), lower->bars.end(), context.sub_bar_open_ms,',
        'if (found == lower->bars.end() || found->timestamp != context.sub_bar_open_ms)',
        'const auto& path = spec->intrabar.lower()->bars;',
        '&& context.input_index + 1 < static_cast<int>(retained_.bars.size())) {',
        'next_input_ms = retained_.bars[static_cast<std::size_t>(context.input_index + 1)].timestamp;',
        '&& !retained_.bars.empty() && calendar_period_for(retained_.script_tf) == CalendarPeriod::DAY',
        'const auto next_stamp = session_period_open_ms(retained_.bars.back().timestamp + 86400000LL,',
    ),
    'src/source/magnifier_intrabars.cpp': (
    ),
}

DRIVER_FORWARD = (
    "inline namespace native_run_spec_v3 { struct NativeRunSpec; }"
)

# The portable run-spec digest is declared in native_run_spec_v3 and defined in
# the consumer translation unit, the only place that owns `hash_spec`, so the
# digest can never drift from the fields the continuation identity folds for a
# spec. That is one sibling inline-namespace block inside the consumer's
# `namespace pineforge`, outside engine_script_run_v19; pin it whole so it stays
# exactly the spec fold seeded as continuation_hash() seeds it, and compare
# epoch ownership as if the allowed block were absent.
CONSUMER_SPEC_DIGEST = """inline namespace native_run_spec_v3 {

uint64_t native_run_spec_digest(const NativeRunSpec& spec) noexcept {
    Fnv f;
    f.run_base = spec.identity.run_number;
    hash_spec(f, spec);
    return f.h;
}

}  // inline namespace native_run_spec_v3
"""

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


def consumer_epoch_source(files):
    """The consumer's engine-epoch scope, with the one allowed sibling block out.

    `native_run_spec_digest` is declared in native_run_spec_v3 and defined in
    this translation unit, next to the `hash_spec` fold it reuses, so it must be
    defined in a native_run_spec_v3 block of its own. Pin that block whole and
    compare epoch ownership as if it were absent (the DRIVER_FORWARD precedent).
    """
    text = files["src/native_execution_consumer.cpp"]
    if text.count(CONSUMER_SPEC_DIGEST) != 1:
        raise ValueError("native_run_spec_digest must be defined in the consumer exactly as the "
                         "native_run_spec_v3 spec fold, seeded like continuation_hash()")
    if "native_run_spec_digest" not in files["include/pineforge/native_run_spec.hpp"]:
        raise ValueError("native_run_spec_digest must be declared in native_run_spec_v3")
    if text.count(CONSUMER_VIEWS_BRIDGE) != 1:
        raise ValueError('context spec digest must preserve its exact v1 bridge')
    epoch = versioned(text.replace(CONSUMER_SPEC_DIGEST, "", 1)
                         .replace(CONSUMER_VIEWS_BRIDGE, "", 1),
                      "pineforge", "engine_script_run_v19")
    if "native_run_spec_digest" in epoch:
        raise ValueError("native_run_spec_digest does not belong to engine_script_run_v19")
    return epoch


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


def historical_tempdir(root: Path):
    """Keep provider extraction outside the repository being checked."""
    temporary_root = Path(tempfile.gettempdir()).resolve()
    repository = root.resolve()
    if temporary_root == repository or repository in temporary_root.parents:
        raise ValueError('system temporary directory is inside the repository')
    return tempfile.TemporaryDirectory(prefix='pineforge-native-fx-introduced-',
                                       dir=temporary_root)


def self_test_tempdir() -> None:
    """Exercise the real provider path against a stand-in repository."""
    global extract_tar, authenticate_headers
    with tempfile.TemporaryDirectory(prefix='pineforge-native-tempdir-test-') as fake:
        root = Path(fake).resolve()
        relative = Path('fixture/host-old/manifest.json')
        provider_dir = root / relative.parent
        provider_dir.mkdir(parents=True)
        (provider_dir / 'headers.tar').write_bytes(b'fixture')
        provider = {'engine_epoch': 'engine_script_run_v1',
                    'manifest': ROOT / relative, 'headers_name': 'headers.tar',
                    'commit': 'fixture', 'tree': 'fixture'}
        original_extract, original_authenticate = extract_tar, authenticate_headers
        def probe_extract(_data, destination):
            location = Path(destination).resolve()
            if root == location or root in location.parents:
                raise ValueError('historical provider extraction entered the repository')
            location.mkdir()
        def probe_authenticate(_destination, _manifest, *, commit, tree):
            return {'files': {}, 'commit': commit, 'tree': tree}
        try:
            extract_tar, authenticate_headers = probe_extract, probe_authenticate
            manifests = authenticate_historical_host_manifests(root, {'fixture': provider})
        finally:
            extract_tar, authenticate_headers = original_extract, original_authenticate
        if 'fixture' not in manifests:
            raise ValueError('historical provider fixture did not execute')


def historical_provider_inputs(root=ROOT, providers=PROVIDERS) -> list:
    """The fixture files authenticate_historical_host_manifests() reads.

    The same provider filter the manifest walk applies: the same-epoch
    controls (v15/v16/v18) and any non host-* provider are never read.
    """
    inputs = []
    for provider in providers.values():
        if provider['engine_epoch'] in ('engine_script_run_v15', 'engine_script_run_v16',
                                        'engine_script_run_v18'):
            continue
        manifest_path = provider['manifest']
        if not manifest_path.parent.name.startswith('host-'):
            continue
        manifest_path = root / manifest_path.relative_to(ROOT)
        inputs.append(manifest_path)
        inputs.append(manifest_path.parent / provider['headers_name'])
    return inputs


def missing_private_fixtures(root=ROOT, providers=PROVIDERS) -> list:
    """The historical-provider fixture inputs this checker reads that the tree lacks."""
    return [path for path in historical_provider_inputs(root, providers) if not path.exists()]


def authenticate_historical_host_manifests(root=ROOT, providers=PROVIDERS):
    """Reuse older authenticated closures without injecting current-only values.

    The frozen v15 provider is a same-epoch control, not evidence that a v15
    value predates its public owner.
    """
    manifests = {}
    with historical_tempdir(root) as temporary:
        for label, provider in providers.items():
            # v15 is the old source-layer provider, frozen v16 is the
            # same-epoch L0 pairing control and frozen v18 is the v19 epoch's
            # rejection-pair provider. None of them can establish when the
            # current FX value was introduced.
            if provider['engine_epoch'] in ('engine_script_run_v15', 'engine_script_run_v16',
                                            'engine_script_run_v18'):
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


def check_native_bar_views(files):
    header = clean(files[NATIVE_BAR_VIEWS_HEADER])
    value = versioned(header, 'pineforge', NATIVE_BAR_VIEWS_NAMESPACE)
    same_names(header, value, TYPE_DEF, 'bar-view public type', NATIVE_BAR_VIEWS_NAMESPACE)
    if namespace_functions(header) != namespace_functions(value):
        raise ValueError('bar-view functions must belong to native_bar_views_v1')
    require(value, ('NativeBarView', 'NativeInstrumentFeedView',
                    'NativeInstrumentFeedReadView', 'NativeBarViews', 'NativeLowerPathSource'),
            NATIVE_BAR_VIEWS_NAMESPACE, r'\b(?:struct|class)\s+NAME\s*\{')
    shapes = (
        ('NativeBarView', 'constBar*data;std::size_tsize;'),
        ('NativeInstrumentFeedView', 'NativeBarViewbars;conststd::int64_t*close_ms;'),
        ('NativeInstrumentFeedReadView',
         'NativeBarViewbars;conststd::int64_t*close_ms;std::size_tclose_count;'),
        ('NativeBarViews', 'constNativeInstrumentFeedView*const*instrument_feeds=nullptr;'
         'std::size_tinstrument_feed_count=0;constNativeLowerPathSource*lower_path=nullptr;'
         'NativeBarViewlower_segment{nullptr,0};'),
    )
    for name, expected in shapes:
        shape = body(value, r'\bstruct\s+' + name + r'\s*\{', name)
        if re.sub(r'\s+', '', shape) != expected:
            raise ValueError(name + ' must preserve its frozen v1 fields')
    functions = ('native_instrument_feed_view', 'normalize_native_run_spec',
                 'validate_native_instrument_feeds', 'validate_native_timeframe_subscriptions',
                 'native_instrument_feeds_digest', 'native_run_spec_digest',
                 'native_intrabar_path_digest')
    require_namespace_functions(value, functions, NATIVE_BAR_VIEWS_NAMESPACE)
    accessor = re.search(r'native_instrument_feed_view\s*\([^;]+;', value, re.S)
    if accessor is None or '=' in accessor.group(0):
        raise ValueError('bar accessor must require its explicit override argument')
    for forbidden in ('stage_instrument_feed_views', 'stage_lower_path_source',
                      'NativeStrategyHost', 'NativeExecutionConsumer'):
        if forbidden in header:
            raise ValueError('bar-view staging/query belongs to the private consumer: ' + forbidden)
    source = versioned(files[NATIVE_BAR_VIEWS_SOURCE], 'pineforge', NATIVE_BAR_VIEWS_NAMESPACE)
    require_namespace_functions(source, ('native_instrument_feed_view', 'native_intrabar_path_digest'),
                                NATIVE_BAR_VIEWS_NAMESPACE)
    for path, expected in BAR_VIEW_READS.items():
        actual = tuple(line.strip() for line in clean(files[path]).splitlines()
                       if re.search(r'(?:\.|->)(?:bars|close_ms)\b', line))
        if actual != expected:
            added = [line for line in actual if line not in expected]
            missing = [line for line in expected if line not in actual]
            raise ValueError(path + ': bar/close access inventory drift; added='
                             + repr(added) + '; missing=' + repr(missing))
    routes = {
        'src/native_execution_consumer.cpp': (
            'candidate, views', 'bar_views());', 'spec->instrument_feeds[subscription.instrument_feed], slot',
            'active_bar_views_.lower_path->segment(begin, end)', 'active_bar_views_.lower_segment = segment'),
        'src/source/pine_security_eval.cpp': (
            'native_instrument_feed_view(installed_symbol_feed(site.feed), slot)',
            'native_instrument_feed_view(feed, nullptr)'),
        'src/source/pine_strategy_host.cpp': (
            'native_instrument_feed_view(feed, slot)', 'stage_instrument_feed_views(nullptr, 0)'),
        'src/source/pine_adapter.cpp': ('normalize_projected_spec(spec)',
            'stage_lower_path_source(', 'make_magnifier_path_source(args.bars, args.n, spec.input_tf,'),
        'src/source/pine_scheduler_native.cpp': ('views.lower_segment',
            'views.lower_path->next_after_segment()'),
    }
    for path, tokens in routes.items():
        for token in tokens:
            if token not in clean(files[path]):
                raise ValueError(path + ': missing view route: ' + token)


def self_test_bar_views():
    files = load(ROOT)
    check_native_bar_views(files)
    mutations = (
        (NATIVE_BAR_VIEWS_HEADER, 'inline namespace native_bar_views_v1',
         'inline namespace native_bar_views_v2'),
        (NATIVE_BAR_VIEWS_HEADER, 'const NativeInstrumentFeedView* borrowed) noexcept;',
         'const NativeInstrumentFeedView* borrowed = nullptr) noexcept;'),
        (NATIVE_BAR_VIEWS_HEADER, 'const NativeLowerPathSource* lower_path',
         'NativeLowerPathSource* lower_path'),
        ('src/source/pine_adapter.cpp', 'normalize_projected_spec(spec)',
         'validate_native_run_spec(spec)'),
        ('src/source/pine_security_eval.cpp', 'native_instrument_feed_view(feed, nullptr)',
         'native_instrument_feed_view(feed, nullptr).bars'),
    )
    for path, before, after in mutations:
        if before not in files[path]:
            raise ValueError('bar-view self-test has a stale mutation: ' + before)
        changed = dict(files)
        changed[path] = files[path].replace(before, after, 1)
        try:
            check_native_bar_views(changed)
        except ValueError:
            continue
        raise ValueError('bar-view guard accepted mutation: ' + before)


def check_texts(files):
    check_native_fx_curve(files)
    check_native_bar_views(files)
    identity = versioned(files[FILES[11]], "pineforge::native_order", "native_order_v1")
    require(identity, ("RunIdentity", "RequestHandle", "Birth"),
            "native_order_v1", r'\b(?:class|struct)\s+NAME\s*\{')
    order = versioned(files[FILES[0]], "pineforge::native_order", "native_order_v7")
    require(order, ("WorkingRequestCore", "Request", "SubmitResult",
                    "AcceptedEvent", "NoEffectEvent", "MatchRejectedEvent",
                    "ExecutionAppliedEvent", "HostSized", "HostSizedKind", "ReverseTo",
                    "RemainingDeferred", "RemainingProjectionDeferred", "NoTarget",
                    "RemainingProjectionNoTarget", "CohortHandle", "BindCohort", "CohortClose",
                    "CohortRoster", "CohortReceipt", "AllowanceDeferred",
                    "OpeningShape", "ExecutionGridPolicy", "ExecutionTerms", "TermsResolvedInput",
                    "TermsResolvedEvent", "NativeCandidatePriceKind",
                    "CashValue", "EquityFraction", "SizeTime", "SizePrice", "Sized",
                    "ScopeClaim", "ScopeBasis", "ScopeFraction",
                    "NativeAnchorRounding", "FromOwnerFill", "ArmContext",
                    "NativeArmVisibility", "WaitForApplied", "ActivationGrid"),
            "native_order_v7", r'\b(?:enum\s+class|class|struct)\s+NAME\s*(?::[^;{]+)?\{')
    # R5 L8b: the activation grid is a host-free value the consumer hands to
    # prepare_trigger; a default-constructed one (no ladder) is the raw rule,
    # so the members keep their inactive defaults and the parameter its default.
    # R5 E16 appended ladder_tick last, inactive at 0.0 like the other two: the
    # run's declared price tick, which names a tick-spelled trailing level's
    # ladder point without quantizing anything onto it.
    activation_grid = re.sub(r'\s+', '', body(order, r'struct\s+ActivationGrid\s*\{', 'activation grid'))
    if activation_grid != 'doubleprice_tick=0.0;boolhalf_up=true;doubleladder_tick=0.0;':
        raise ValueError(
            'ActivationGrid must keep {price_tick = 0.0, half_up = true, ladder_tick = 0.0}')
    # R5 L3: prepare_trigger's direct form apply_trigger takes it the same way.
    if len(re.findall(r'\bconst\s+ActivationGrid\s*&\s*grid\s*=\s*\{\s*\}\s*\)', order)) != 2:
        raise ValueError(
            'prepare_trigger and apply_trigger must each take a defaulted ActivationGrid last')
    require(order, ("RequestOrigin", "MarginCallEvent", "RiskLimitKind", "NativeRiskEvent"),
            "native_order_v7", r'\b(?:enum\s+class|class|struct)\s+NAME\s*(?::[^;{]+)?\{')
    require(order, ("CommandEvent", "ExecutionPlan", "OrderIntent", "Remaining",
                    "RemainingProjection", "Allowance", "ReductionSize", "SizeBasis"),
            "native_order_v7", r'\busing\s+NAME\s*=')
    for token in ('operator==(CohortHandle', 'operator<(CohortHandle',
                  'struct hash<pineforge::native_order::CohortHandle>'):
        if token not in files[FILES[0]]:
            raise ValueError('CohortHandle requires C++17 equality/order/hash support')
    require_exact_alias(
        order, "OrderIntent", "std::variant<Flatten,Reduce,Transact,ReverseTo,HostSized,Sized>",
        "native_order_v7")
    require_exact_alias(
        order, "ReductionSize", "std::variant<ExplicitUnits,OwnerOpenedUnits,ScopeFraction>",
        "native_order_v7")
    require_exact_alias(
        order, "SizeBasis", "std::variant<CashValue,EquityFraction>", "native_order_v7")
    require_exact_alias(
        order, "Remaining",
        "std::variant<RemainingUnbound,RemainingFlattenAll,RemainingUnits,RemainingDeferred,NoTarget>",
        "native_order_v7")
    require_exact_alias(
        order, "RemainingProjection",
        "std::variant<RemainingProjectionUnbound,RemainingProjectionFlattenAll,"
        "RemainingProjectionUnits,RemainingProjectionDeferred,RemainingProjectionNoTarget>", "native_order_v7")
    require_exact_alias(
        order, "Allowance",
        "std::variant<AllowanceUnset,AllowanceUnits,AllowanceAllScope,AllowanceDeferred>",
        "native_order_v7")
    require_exact_alias(
        order, "ExecutionPlan",
        "std::variant<execution::Flatten,order_action::Reduce,order_action::Transact,"
        "execution::ReverseTo>", "native_order_v7")
    execution_terms = body(order, r'struct\s+ExecutionTerms\s*\{', 'execution terms')
    if ('ExecutionGridPolicygrid_policy=ExecutionGridPolicy::SnapToGrid;'
            not in re.sub(r'\s+', '', execution_terms)):
        raise ValueError('ExecutionTerms omits its default grid policy')
    sized = re.sub(r'\s+', '', body(order, r'struct\s+Sized\s*\{', 'native sizing'))
    for pinned in ('SizeTimetime=SizeTime::AtMatch;',
                   'SizePriceprice=SizePrice::Resolved;',
                   'ExecutionGridPolicygrid_policy=ExecutionGridPolicy::SnapToGrid;',
                   'boolreserve_percent_fee=false;'):
        if pinned not in sized:
            raise ValueError('Sized omits a pinned default: ' + pinned)
    # R5 L7b: the anchor rounding is appended last on FromOwnerFill with a Raw
    # default, so every existing {offset, ticks} initializer keeps its
    # meaning and every Raw anchor keeps its continuation digest.
    anchor = re.sub(r'\s+', '', body(order, r'struct\s+FromOwnerFill\s*\{', 'owner-fill anchor'))
    if anchor != ('doubleoffset=0.0;boolticks=false;'
                  'NativeAnchorRoundingrounding=NativeAnchorRounding::Raw;'):
        raise ValueError('FromOwnerFill must keep its member order and Raw rounding default')
    anchor_rounding = body(order, r'enum\s+class\s+NativeAnchorRounding\s*:[^{]+\{',
                           'anchor rounding')
    if re.findall(r'(\w+)\s*=\s*(\d+)', anchor_rounding) != [
            ('Raw', '0'), ('HalfUp', '1'), ('Directional', '2')]:
        raise ValueError('NativeAnchorRounding must keep Raw=0, HalfUp=1, Directional=2')
    # R5 L7b: the arm visibility is appended last on WaitForApplied with a
    # Working default (the established book), so every {parent} initializer
    # keeps its meaning and every Working child keeps its digest.
    # R5 N13: the first-match rule and the arm scope are appended after it
    # with the established defaults (AtArmPrint, OwnerLot), so every {parent}
    # and {parent, visibility} initializer keeps its meaning and every
    # default child keeps its digest.
    wait = re.sub(r'\s+', '', body(order, r'struct\s+WaitForApplied\s*\{', 'wait-for-applied'))
    if wait != ('RequestHandleparent;'
                'NativeArmVisibilityvisibility=NativeArmVisibility::Working;'
                'NativeArmFirstMatchfirst_match=NativeArmFirstMatch::AtArmPrint;'
                'NativeArmScopescope=NativeArmScope::OwnerLot;'):
        raise ValueError('WaitForApplied must keep its member order and its Working / '
                         'AtArmPrint / OwnerLot defaults')
    first_match = body(order, r'enum\s+class\s+NativeArmFirstMatch\s*:[^{]+\{', 'arm first match')
    if re.findall(r'(\w+)\s*=\s*(\d+)', first_match) != [
            ('AtArmPrint', '0'), ('AfterArmPrint', '1')]:
        raise ValueError('NativeArmFirstMatch must keep AtArmPrint=0, AfterArmPrint=1')
    arm_scope = body(order, r'enum\s+class\s+NativeArmScope\s*:[^{]+\{', 'arm scope')
    if re.findall(r'(\w+)\s*=\s*(\d+)', arm_scope) != [('OwnerLot', '0'), ('Book', '1')]:
        raise ValueError('NativeArmScope must keep OwnerLot=0, Book=1')
    visibility = body(order, r'enum\s+class\s+NativeArmVisibility\s*:[^{]+\{', 'arm visibility')
    if re.findall(r'(\w+)\s*=\s*(\d+)', visibility) != [
            ('Working', '0'), ('PendingUntilArmed', '1')]:
        raise ValueError('NativeArmVisibility must keep Working=0, PendingUntilArmed=1')
    arm_context = re.sub(r'\s+', '', body(order, r'struct\s+ArmContext\s*\{', 'arm context'))
    if arm_context != 'std::optional<double>price_tick;AnchoredLevelResolverresolve_level;':
        raise ValueError('ArmContext must carry exactly the price tick and the level resolver')
    if not re.search(r'\busing\s+AnchoredLevelResolver\s*=\s*std::function\s*<\s*std::optional\s*<'
                     r'\s*double\s*>\s*\(', order):
        raise ValueError('AnchoredLevelResolver must be a native_order_v7 optional<double> callable')
    fraction = re.sub(r'\s+', '', body(order, r'struct\s+ScopeFraction\s*\{', 'scope fraction'))
    for pinned in ('ScopeClaimclaim=ScopeClaim::Gross;',
                   'ScopeBasisbasis=ScopeBasis::AtMatch;'):
        if pinned not in fraction:
            raise ValueError('ScopeFraction omits a pinned default: ' + pinned)
    # R5 V19-B: the chain root is appended after origin (every existing
    # aggregate initializer keeps its meaning), and a trail's arm ordinal is
    # appended to both of its tracking states. R5 V19-D appends the queue
    # priority (0: the handle's incarnation) and the carried book binding
    # after the root, for the same reason.
    definition = re.sub(r'\s+', '', body(order, r'struct\s+RequestDefinition\s*\{',
                                          'request definition'))
    if not definition.endswith('RequestOriginorigin=RequestOrigin::Host;'
                               'std::optional<RequestHandle>root;'
                               'uint64_tpriority=0;'
                               'std::optional<PositionNonflat>kept_binding;'):
        raise ValueError('RequestDefinition must end with origin, the chain root, the '
                         'queue priority, then the kept binding')
    options = re.sub(r'\s+', '', body(order, r'struct\s+ReplaceOptions\s*\{',
                                       'replace options'))
    if options != ('boolretain_trigger_state=false;boolkeep_handle=false;'
                   'boolkeep_binding=false;'):
        raise ValueError('ReplaceOptions must keep retain_trigger_state, then keep_handle '
                         'and keep_binding, every one off by default')
    for state, first in (('TrailTrack', 'doublebest=0.0;'),
                         ('TrailActive', 'doublebest_at_trigger=0.0;')):
        shape = re.sub(r'\s+', '', body(order, r'struct\s+' + state + r'\s*\{', state))
        if shape != first + 'uint64_tactivation_ordinal=0;':
            raise ValueError(state + ' must keep its best, then the arm ordinal')
    live = re.sub(r'\s+', '', body(order, r'struct\s+LiveRequest\s*\{', 'live request'))
    for pinned in ('std::optional<double>sizing_units;',
                   'std::optional<double>sizing_scope;',
                   'std::optional<double>sizing_price;'):
        if pinned not in live:
            raise ValueError('LiveRequest omits a placement-time sizing member: ' + pinned)
    context = re.sub(r'\s+', '', body(order, r'struct\s+CommandContext\s*\{', 'command context'))
    for pinned in ('std::optional<double>sizing_units;',
                   'std::optional<double>sizing_scope;',
                   'std::optional<double>sizing_price;',
                   'boolsizing_admissible=true;'):
        if pinned not in context:
            raise ValueError('CommandContext omits a placement-time sizing member: ' + pinned)
    require_namespace_functions(order, ("to_execution_plan",), "native_order_v7")
    required_order_members = (
        (r'\bPreparation<PreparedMutation>\s+prepare_terms\s*\(', "prepare_terms"),
        (r'\bstatic\s+Allowance\s+evaluated_allowance\s*\(', "evaluated_allowance"),
        (r'\bstatic\s+bool\s+effective_host_units\s*\(', "effective_host_units"),
        (r'\bCohortHandle\s+cohort_open\s*\(', "cohort_open"),
        (r'\bvoid\s+cohort_add\s*\(', "cohort_add"),
        (r'\bvoid\s+cohort_remove\s*\(', "cohort_remove"),
    )
    for pattern, name in required_order_members:
        if len(re.findall(pattern, order)) != 1:
            raise ValueError(name + " must be a native_order_v7 WorkingRequestCore member")
    if re.search(r'\b(?:class|struct)\s+RunIdentity\s*\{', order):
        raise ValueError("RunIdentity must remain in native_order_v1, not native_order_v7")
    order_src = versioned(files[FILES[1]], "pineforge::native_order", "native_order_v7")
    require(order_src, ("WorkingRequestCore::reset", "WorkingRequestCore::find_live",
                        "WorkingRequestCore::prepare_terms",
                        "WorkingRequestCore::evaluated_allowance",
                        "WorkingRequestCore::effective_host_units",
                        "WorkingRequestCore::cohort_open", "WorkingRequestCore::cohort_add",
                        "WorkingRequestCore::cohort_remove"),
            "native_order_v7", r'\bNAME\s*\(')

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

    spec = versioned(files[FILES[4]], "pineforge", "native_run_spec_v3")
    require(spec, ("NativeRunSpec", "NativeRunSpecValidation", "NativeRunSpecError",
                   "NativeRunSpecField", "IntrabarPath", "SampleEligibility", "synthesized",
                   "NativeSlotLabelPolicy", "NativePathOrder",
                   "NativeFeedTolerance", "NativeReportPolicy",
                   "NativeTimeframeSubscription", "NativeMarginModel",
                   "NativeLiquidationSizing", "NativeLiquidationCheck",
                   "NativeMarginEquityBasis", "NativeLiquidationLevelBase",
                   "NativeCalculationTrigger", "NativeOpenBarView",
                   "NativeLossLimit", "NativeRiskDay", "NativeRiskAction",
                   "NativeRiskLimits", "NativeSeriesSource", "NativeAuxiliaryFeed",
                   "NativeEventRetention"),
            "native_run_spec_v3",
            r'\b(?:enum\s+class|struct)\s+NAME\s*(?::[^;{]+)?\{')
    require_namespace_functions(
        spec, ("validate_native_run_spec", "normalize_native_run_spec",
               "validate_native_timeframe_subscriptions",
               "validate_native_auxiliary_feed",
               "native_intrabar_path_digest", "native_timeframe_subscriptions_digest",
               "native_auxiliary_feed_digest",
               "native_margin_model_digest", "native_risk_limits_digest"),
        "native_run_spec_v3")
    # R5 lane L12 (2.ii c) renamed the feed-tolerance surface. The deprecated
    # spellings stay compilable for existing hosts and the source adapter, so
    # pin them as aliases of the neutral names rather than as definitions.
    for alias in ('using NativeLegacyTolerance = NativeFeedTolerance;',
                  'LegacyTolerant = FeedTolerant,'):
        if alias not in spec:
            raise ValueError('native_run_spec_v3 dropped a deprecated alias: ' + alias)
    if files[FILES[5]].count(SPEC_VIEWS_BRIDGE) != 1:
        raise ValueError('context spec validation/digest must preserve their exact v1 bridge')
    spec_src = versioned(files[FILES[5]].replace(SPEC_VIEWS_BRIDGE, '', 1),
                         "pineforge", "native_run_spec_v3")
    require_namespace_functions(
        spec_src, ("validate_native_run_spec", "normalize_native_run_spec",
                   "validate_native_timeframe_subscriptions",
                   "validate_native_auxiliary_feed", "validate_native_instrument_feeds",
                   "native_intrabar_path_digest", "native_timeframe_subscriptions_digest",
                   "native_auxiliary_feed_digest", "native_instrument_feeds_digest",
                   "native_margin_model_digest", "native_risk_limits_digest"),
        "native_run_spec_v3")
    run_spec = body(spec, r'struct\s+NativeRunSpec\s*\{', 'native run spec')
    if ('std::stringinput_tf;std::stringscript_tf;booltimeframe_undetected=false;'
            not in re.sub(r'\s+', '', run_spec)):
        raise ValueError('native_run_spec_v3 requires its explicit undetected-timeframe field')
    compact_spec = re.sub(r'\s+', '', run_spec)
    if not re.search(r'\benum\s+class\s+NativeAbortReporting\s*:', spec):
        raise ValueError('native_run_spec_v3 omits NativeAbortReporting')
    for member in (
            'NativeSlotLabelPolicyslot_label_policy=NativeSlotLabelPolicy::Canonical;',
            'NativeFeedTolerancelegacy_tolerance=NativeFeedTolerance::None;',
            'NativePathOrderpath_order=NativePathOrder::Auto;',
            'NativeAbortReportingabort_reporting=NativeAbortReporting::Error;',
            'NativeReportPolicyreport_policy=NativeReportPolicy::HostRecorded;',
            'boolreport_open_position_at_end=false;',
            'std::vector<NativeTimeframeSubscription>subscriptions;',
            'std::optional<NativeMarginModel>margin;',
            'NativeCalculationTriggercalculation=NativeCalculationTrigger::BarClose;',
            'std::uint32_tmax_recalculations_per_point=8;',
            'NativeOpenBarViewopen_bar_view=NativeOpenBarView::Complete;',
            'std::optional<NativeRiskLimits>risk;',
            'std::optional<NativeAuxiliaryFeed>auxiliary_feed;',
            'std::vector<NativeInstrumentFeed>instrument_feeds;',
            'NativeEventRetentionevent_retention=NativeEventRetention::Window;'):
        if member not in compact_spec:
            raise ValueError('native_run_spec_v3 omits required policy member: ' + member)
    subscription = body(spec, r'struct\s+NativeTimeframeSubscription\s*\{',
                        'native timeframe subscription')
    # XSYM-D appends the instrument key last: every member before it keeps its
    # order, shape and default.
    if (re.sub(r'\s+', '', subscription)
            != 'std::stringtf;std::vector<Bar>authoritative_bars;boollookahead=false;'
               'boolgaps=false;NativeSeriesSourcesource=NativeSeriesSource::Input;'
               'std::stringinstrument;'):
        raise ValueError('native timeframe subscription must preserve its member order and shape')
    # The series judge has exactly three spellings: the established one, the
    # one that also names the run's auxiliary feed, and the one that also
    # names its instrument feeds (XSYM-D). All three are declared.
    if len(re.findall(r'\bvalidate_native_timeframe_subscriptions\s*\(', spec)) != 3:
        raise ValueError('native_run_spec_v3 must declare the three series validation overloads')
    instrument_feed = body(spec, r'struct\s+NativeInstrumentFeed\s*\{', 'native instrument feed')
    if (re.sub(r'\s+', '', instrument_feed)
            != 'std::stringinstrument;std::stringtf;std::vector<Bar>bars;'
               'std::vector<std::int64_t>close_ms;std::vector<NativeInstrumentColumn>columns;'):
        raise ValueError('native instrument feed must preserve its member order and shape')
    instrument_column = body(spec, r'struct\s+NativeInstrumentColumn\s*\{',
                             'native instrument column')
    if re.sub(r'\s+', '', instrument_column) != 'std::stringname;std::vector<double>values;':
        raise ValueError('native instrument column must preserve its member order and shape')
    auxiliary_feed = body(spec, r'struct\s+NativeAuxiliaryFeed\s*\{', 'native auxiliary feed')
    if re.sub(r'\s+', '', auxiliary_feed) != 'std::stringtf;std::vector<Bar>bars;':
        raise ValueError('native auxiliary feed must preserve its member order and shape')
    # R5 V19-B: the retention policy is appended last with its Window default,
    # which folds nothing, and its values are the C words.
    retention = body(spec, r'enum\s+class\s+NativeEventRetention\s*:\s*std::uint32_t\s*\{',
                     'native event retention')
    if re.sub(r'\s+', '', retention) != 'Window=0,Full=1,Commands=2,':
        raise ValueError('native event retention must keep Window=0, Full=1, Commands=2')
    series_source = body(spec, r'enum\s+class\s+NativeSeriesSource\s*:\s*std::uint8_t\s*\{',
                         'native series source')
    if re.sub(r'\s+', '', series_source) != 'Input=0,AuxiliaryFeed=1,InstrumentFeed=2,':
        raise ValueError('native series source must preserve its enumerators and values')
    margin = body(spec, r'struct\s+NativeMarginModel\s*\{', 'native margin model')
    if (re.sub(r'\s+', '', margin)
            != 'doubleinitial_long=0.0;doubleinitial_short=0.0;'
               'std::optional<double>maintenance_long;std::optional<double>maintenance_short;'
               'NativeLiquidationSizingsizing=NativeLiquidationSizing::RestoreMinimum;'
               'doubleshortfall_multiple=1.0;std::optional<double>liquidation_min_units;'
               'NativeLiquidationCheckcheck=NativeLiquidationCheck::PathAdverseExtreme;'
               'NativeMarginEquityBasisbasis=NativeMarginEquityBasis::MarkedEquity;'
               'NativeLiquidationLevelBaselevel_base='
               'NativeLiquidationLevelBase::MarkedEquity;'
               'std::stringliquidation_label;std::stringliquidation_comment;'):
        raise ValueError('native margin model must preserve its member order and shape')
    risk = body(spec, r'struct\s+NativeRiskLimits\s*\{', 'native risk limits')
    if (re.sub(r'\s+', '', risk)
            != 'std::optional<NativeLossLimit>max_drawdown;'
               'std::optional<NativeLossLimit>max_intraday_loss;'
               'std::optional<std::uint32_t>max_consecutive_loss_days;'
               'std::optional<std::uint32_t>max_fills_per_day;'
               'NativeRiskDayday_basis=NativeRiskDay::SessionDay;'
               'NativeRiskActionaction=NativeRiskAction::BlockOpenings;'):
        raise ValueError('native risk limits must preserve their member order and shape')
    loss_limit = body(spec, r'struct\s+NativeLossLimit\s*\{', 'native loss limit')
    if re.sub(r'\s+', '', loss_limit) != 'doublevalue=0.0;boolpercent=false;':
        raise ValueError('native loss limit must preserve its member order and shape')
    fields = body(spec, r'enum\s+class\s+NativeRunSpecField\s*:\s*std::uint8_t\s*\{',
                  'native run spec fields')
    for field in ('TimeframeUndetected', 'SlotLabelPolicy', 'LegacyTolerance', 'AbortReporting',
                  'PathOrder', 'ReportPolicy', 'SubscriptionTimeframe', 'SubscriptionBars',
                  'MarginModel', 'MarginInitial', 'MarginMaintenance', 'MarginSizing',
                  'MarginShortfallMultiple', 'MarginMinUnits', 'MarginCheck',
                  'MarginEquityBasis', 'MarginLevelBase',
                  'Calculation', 'OpenBarView',
                  'RiskLimits', 'RiskDrawdown', 'RiskIntradayLoss', 'RiskLossDays',
                  'RiskFillsPerDay', 'RiskDayBasis', 'RiskAction',
                  'AuxiliaryFeedTimeframe', 'AuxiliaryFeedBars', 'SubscriptionSource',
                  'EventRetention'):
        if not re.search(r'\b' + field + r'\b', fields):
            raise ValueError('native_run_spec_v3 omits the field tag: ' + field)
    errors = body(spec, r'enum\s+class\s+NativeRunSpecError\s*:\s*std::uint8_t\s*\{',
                  'native run spec errors')
    for error in ('InvalidUndetectedTimeframe', 'UnknownSlotLabelPolicy',
                  'UnknownLegacyTolerance', 'UnknownAbortReporting',
                  'UnknownIntrabarSampleEligibility', 'UnknownPathOrder',
                  'UnknownReportPolicy',
                  'InvalidSubscriptionTimeframe', 'SubscriptionFinerThanInput',
                  'DuplicateSubscriptionTimeframe', 'UnorderedSubscriptionBars',
                  'SubscriptionWithoutTimeframe', 'MarginModelConflict',
                  'UnknownLiquidationSizing', 'UnknownLiquidationCheck',
                  'UnknownMarginEquityBasis', 'UnknownLiquidationLevelBase',
                  'UnknownCalculationTrigger', 'UnknownOpenBarView',
                  'UnknownRiskDay', 'UnknownRiskAction', 'ZeroRiskLimit',
                  'MarginSideUndeclared',
                  'InvalidAuxiliaryFeedTimeframe', 'AuxiliaryFeedNotFinerThanInput',
                  'UnorderedAuxiliaryFeedBars', 'InvalidAuxiliaryFeedBar',
                  'AuxiliaryFeedWithoutTimeframe', 'UnknownSeriesSource',
                  'SubscriptionWithoutAuxiliaryFeed', 'SubscriptionFinerThanAuxiliaryFeed',
                  'UnknownEventRetention', 'AbortedHostNoReusableRunSpec',
                  'SessionKeyChangedOnReuse', 'RunNumberNotAboveConsumedHighWater'):
        if not re.search(r'\b' + error + r'\b', errors):
            raise ValueError('native_run_spec_v3 omits the validation error: ' + error)
    if ('spec.timeframe_undetected' not in spec_src
            or 'InvalidUndetectedTimeframe' not in spec_src
            or 'spec.slot_label_policy' not in spec_src
            or 'spec.legacy_tolerance' not in spec_src
            or 'spec.path_order' not in spec_src
            or 'spec.abort_reporting' not in spec_src
            or 'spec.report_policy' not in spec_src
            or 'spec.subscriptions' not in spec_src
            or 'spec.calculation' not in spec_src
            or 'spec.open_bar_view' not in spec_src
            or 'SubscriptionFinerThanInput' not in spec_src
            or 'if (subscription.gaps)' not in spec_src
            or 'if (subscription.source != NativeSeriesSource::Input) {' not in spec_src
            or 'spec.auxiliary_feed' not in spec_src
            or 'AuxiliaryFeedNotFinerThanInput' not in spec_src
            or 'SubscriptionFinerThanAuxiliaryFeed' not in spec_src
            or 'spec.margin' not in spec_src
            or 'valid_margin_equity_basis' not in spec_src
            or 'valid_liquidation_level_base' not in spec_src
            or 'MarginModelConflict' not in spec_src
            or 'spec.risk' not in spec_src
            or 'ZeroRiskLimit' not in spec_src
            or 'spec.event_retention' not in spec_src
            or 'lower->sample_eligibility' not in spec_src):
        raise ValueError('native run-spec validation omits an explicit compatibility rule')
    intrabar = body(spec, r'struct\s+IntrabarPath\s*\{', 'intrabar path')
    if ('SampleEligibilitysample_eligibility='
            'SampleEligibility::ContinuousSegments;' not in re.sub(r'\s+', '', intrabar)):
        raise ValueError('native intrabar path omits its default sample-eligibility policy')
    intrabar_fields = body(spec, r'enum\s+class\s+NativeRunSpecField\s*:\s*std::uint8_t\s*\{',
                           'native run spec fields')
    if not re.search(r'\bIntrabarSampleEligibility\b', intrabar_fields):
        raise ValueError('native run-spec field tags omit intrabar sample eligibility')
    if 'u(static_cast<std::uint64_t>(lower->sample_eligibility));' not in spec_src:
        raise ValueError('native intrabar path digest omits sample eligibility')
    synthesized = body(intrabar, r'struct\s+synthesized\s*\{', 'synthesized intrabar path')
    compact_synthesized = re.sub(r'\s+', '', synthesized)
    for member in ('intsamples=4;', 'MagnifierDistributiondistribution='
                   'MagnifierDistribution::ENDPOINTS;',
                   'boolvolume_weighted=false;',
                   'intvolume_weighted_min_samples=2;',
                   'intvolume_weighted_max_samples=64;'):
        if member not in compact_synthesized:
            raise ValueError('synthesized intrabar path omits sampling member: ' + member)
    if ('std::variant<none,lower_tf,synthesized>' not in re.sub(r'\s+', '', intrabar)
            or 'const synthesized* synthesized_path() const noexcept' not in intrabar):
        raise ValueError('native intrabar path omits its synthesized variant')
    for token in ('synthesized->samples', 'synthesized->distribution',
                  'synthesized->volume_weighted',
                  'synthesized->volume_weighted_min_samples',
                  'synthesized->volume_weighted_max_samples'):
        if token not in spec_src:
            raise ValueError('native synthesized intrabar digest/validation omits: ' + token)
    for fold in ('i(synthesized->samples);',
                 'u(static_cast<std::uint64_t>(synthesized->distribution));',
                 'u(synthesized->volume_weighted ? 1u : 0u);',
                 'i(synthesized->volume_weighted_min_samples);',
                 'i(synthesized->volume_weighted_max_samples);'):
        if fold not in spec_src:
            raise ValueError('native synthesized intrabar digest omits: ' + fold)

    driver_text = files[FILES[6]]
    if driver_text.count(DRIVER_FORWARD) != 1:
        raise ValueError("market_driver.hpp must forward-declare NativeRunSpec "
                         "in native_run_spec_v3 outside native_driver_v5")
    driver_clean = clean(driver_text)
    driver_owner = body(driver_clean, r'namespace\s+pineforge\s*\{', "pineforge")
    driver = body(driver_owner, r'inline\s+namespace\s+native_driver_v5\s*\{',
                  "native_driver_v5")
    if DRIVER_FORWARD in driver or "native_run_spec_v3" in driver:
        raise ValueError("NativeRunSpec forward declaration must stay outside native_driver_v5")
    if re.search(r'\bstruct\s+NativeRunSpec\s*\{', driver_clean):
        raise ValueError("NativeRunSpec definition does not belong to native_driver_v5")
    # Compare ownership as if the allowed forward declaration were absent.
    driver_without_forward = driver_clean.replace(DRIVER_FORWARD, "", 1)
    versioned(driver_without_forward, "pineforge", "native_driver_v5")
    require(driver, ("NativeCoordinate", "NativeDriverPoint", "NativeDriverStatistics",
                     "NativeDecisionContext",
                     "NativeInputPreflightResult", "INativeDriverSink"),
            "native_driver_v5", r'\b(?:class|struct)\s+NAME\s*\{')
    decision = body(driver, r'struct\s+NativeDecisionContext\s*\{', 'decision context')
    for member in ('intsub_index=0;', 'intsub_count=1;', 'boolis_terminal_sub_bar=true;',
                   'int64_tsub_bar_open_ms=0;', 'int64_tscript_bar_open_ms=0;',
                   'NativeDriverStatisticsdriver_statistics{};'):
        if member not in re.sub(r'\s+', '', decision):
            raise ValueError('native_driver_v5 decision context omits intrabar field: ' + member)
    require_namespace_functions(
        driver, ("native_bar_structurally_valid", "preflight_native_inputs"),
        "native_driver_v5")
    if 'kNativeConsumerSemanticVersion = "native-consumer/v9"' not in driver_text:
        raise ValueError("consumer semantic marker must be native-consumer/v9")
    provenance = body(driver, r'enum\s+class\s+NativePriceProvenance\s*:[^{]+\{', 'price provenance')
    expected_provenance = [('Confirmed', '0'), ('ObservedPrint', '1'), ('ModeledOHLCOpen', '2'),
        ('ModeledOHLCClose', '3'), ('CarriedOpen', '4'), ('AfterCalculationClose', '5'),
        ('PartialFinalized', '6'), ('Calculation', '7'), ('CurrentExecution', '8')]
    if re.findall(r'(\w+)\s*=\s*(\d+)', provenance) != expected_provenance:
        raise ValueError('driver provenance must preserve 0..7 and append only CurrentExecution=8')
    if 'kNativeDriverSemanticVersion = "native-driver/v5"' not in driver_text:
        raise ValueError('driver semantic marker must be native-driver/v5')
    driver_src = versioned(files[FILES[7]], "pineforge", "native_driver_v5")
    require_namespace_functions(
        driver_src, ("native_bar_structurally_valid", "preflight_native_inputs"),
        "native_driver_v5")
    for token in ('spec.slot_label_policy == NativeSlotLabelPolicy::FeedTolerant',
                  'NativeFeedTolerance::BatchStructuralBars',
                  'NativeFeedTolerance::WarmupNonNegativeOHLC',
                  'NativeInputPreflightError::TimestampDeltaOverflow'):
        if token not in driver_src:
            raise ValueError('native driver omits feed-tolerance preflight token: ' + token)

    consumer_src = consumer_epoch_source(files)
    # R5 L8b: the consumer hands the run's activation grid to the core at both
    # trigger sites (the path matcher and the trail observer), and the grid is
    # active only under QuantizeFillsAndTriggers (grid_threshold's own gate).
    if consumer_src.count('activation_grid(*spec)') != 2:
        raise ValueError('native consumer must hand activation_grid(*spec) to both trigger sites')
    if 'const auto threshold = grid_threshold(spec);' not in consumer_src:
        raise ValueError('activation_grid must derive from grid_threshold, the matcher gate')
    if 'native_margin_model_digest(*spec.margin)' not in consumer_src:
        raise ValueError('native consumer omits the conditional margin-model digest fold')
    if 'native_risk_limits_digest(*spec.risk)' not in consumer_src:
        raise ValueError('native consumer omits the conditional risk-limits digest fold')
    for fold in ('f.u(static_cast<uint64_t>(spec.slot_label_policy));',
                 'f.u(static_cast<uint64_t>(spec.legacy_tolerance));',
                 'f.u(static_cast<uint64_t>(spec.abort_reporting));',
                 'f.u(static_cast<uint64_t>(spec.path_order));',
                 'f.u(static_cast<uint64_t>(terms.grid_policy));'):
        if fold not in consumer_src:
            raise ValueError('native continuation hash omits compatibility policy: ' + fold)
    # R5 L3b placement-time sizing. Each fold is conditional, so a run that
    # never sets the member keeps its established continuation identity; the
    # checker only proves the fold exists at all.
    for fold in ('if (live.sizing_units) { f.u(1); f.d(*live.sizing_units); }',
                 'if (live.sizing_scope) { f.u(2); f.d(*live.sizing_scope); }',
                 'if (live.sizing_price) { f.u(3); f.d(*live.sizing_price); }',
                 'f.u(static_cast<uint64_t>(payload.price));',
                 'f.u(static_cast<uint64_t>(fraction->basis));'):
        if fold not in consumer_src:
            raise ValueError('native continuation hash omits placement-time sizing: ' + fold)
    # R5 L7b anchored legs. The anchor rounding folds only when it is set,
    # so every Raw anchor keeps its established continuation identity; the
    # arm reads the run's price tick through the core's ArmContext.
    for fold in ('if (anchor->rounding != native_order::NativeAnchorRounding::Raw) {',
                 'f.u(static_cast<uint64_t>(anchor->rounding));',
                 'if (const auto* spec = spec_ptr()) arm.price_tick = spec->price_tick;',
                 'return host->resolve_anchored_level(view);',
                 'next_timeline_ordinal_, arm);',
                 'if (value.visibility != native_order::NativeArmVisibility::Working) {',
                 'f.u(static_cast<uint64_t>(value.visibility));',
                 'if (value.first_match != native_order::NativeArmFirstMatch::AtArmPrint) {',
                 'f.u(static_cast<uint64_t>(value.first_match));',
                 'if (value.scope != native_order::NativeArmScope::OwnerLot) {',
                 'f.u(static_cast<uint64_t>(value.scope));',
                 '&& !armed_after_print_here(*live);',
                 'bool hidden_until_armed(const native_order::LiveRequest& live) noexcept {',
                 'if (hidden_until_armed(live)) continue;'):
        if fold not in consumer_src:
            raise ValueError('native consumer omits the anchored-leg fold/arm token: ' + fold)
    for token in ('lower->sample_eligibility',
                  'IntrabarPath::SampleEligibility::DistributionSamples',
                  'const auto* synthesized = spec ? spec->intrabar.synthesized_path() : nullptr;',
                  'if (distribution_samples || sample_index == 0)',
                  'driver_statistics_.sample_ticks_processed',
                  'bool execution_terms_grid_representable(',
                  'bool path_uses_high_first(',
                  'path_uses_high_first(sub, spec->path_order)',
                  'const bool intrabar_points_drive_floor = kind == InputContribution::ConfirmedBar',
                  'input_callback_context_', 'hash_input_context',
                  'tick_callback_context_', 'hash_tick_context',
                  'invoke_tick_callback(engine, tick_bar, tick_context)',
                  'staged_ingress_fx_', 'if (failed() && !recoverable_abort())',
                  'NativeLiquidationCheck::PathAdverseExtremeMark',
                  'host->resolve_margin_requirement(view)',
                  'host->margin_check_allowed(point)',
                  'NativeMarginEquityBasis::MarkedEquityBeforeOpenCommission',
                  'NativeLiquidationLevelBase::RealizedOnly',
                  'bool calc_timing_on(const NativeRunSpec& spec) noexcept {',
                  'if (calc_timing_on(spec)) {',
                  'NativeCalculationReason::BarClose',
                  'NativeCalculationReason::OrderFill',
                  'host->on_native_sub_bar(sub, presented);',
                  'NativeOpenBarView::OpenOnly',
                  # L6c: the declared series are registered AFTER the host's
                  # own run-begin work, the declaration hook is legal only
                  # inside it, the kernel's own wiring is what the in-run
                  # mutation guard stands down for, and gaps clears the
                  # series where it delivers nothing.
                  'in_run_begin_ = true;',
                  'wiring_subscriptions_ = true;',
                  'clear_if_gapped(subscription);',
                  'if (subscription.gaps) subscription.latest.reset();',
                  # N7: the auxiliary feed folds into the spec identity only
                  # where one is declared, a series built from it has its own
                  # feed cursor in the continuation fold, a stream's appended
                  # bars are hashed, and the slice is cut by the accepted
                  # input's own period end.
                  'if (spec.auxiliary_feed) {',
                  'f.u(native_auxiliary_feed_digest(*spec.auxiliary_feed));',
                  'if (subscription.auxiliary) f.u(subscription.auxiliary_cursor);',
                  'f.u(auxiliary_appended_digest_);',
                  'if (feed_bar.timestamp >= input_period_end_ms) break;'):
        if token not in consumer_src:
            raise ValueError('native consumer omits staged/intrabar policy token: ' + token)

    host = versioned(files[FILES[8]], "pineforge", "engine_script_run_v19")
    require(host, ("NativeStrategyHost", "NativeStateView", "NativeLifecycleKind",
                   "NativeFailure", "NativeFailureContext", "NativeInRunCause",
                   "NativeInRunRecipient", "NativeInRunCursor", "NativeMarketEvent",
                   "NativeSetupResult", "NativePhysicalPosition", "NativeAccountObservation",
                   "NativeCurrentPriceRule", "NativeCurrentQuoteKind", "NativeCurrentPointView",
                   "NativeTrailState",
                   "NativeCurrentRefusal", "NativeCurrentExecution", "NativeCurrentExecutionPreview",
                   "NativeExecutionTermsFacts", "NativePrecommitView",
                   "NativePrecommitVerdict", "NativeFxCurveSetupResult", "NativeBeginArgs",
                   "NativeInputContext", "NativeTickContext",
                   "NativeTimeframeBarContext", "NativeMarginCallView",
                   "NativeMarginCheckKind", "NativeMarginCheckPoint",
                   "NativeMarginRequirementView", "NativeMarginDecision",
                   "NativeCalculationReason", "NativeRiskState",
                   "NativeAnchoredTrigger", "NativeAnchoredLevelView",
                   "NativeOpenLot"),
            "engine_script_run_v19",
            r'\b(?:enum\s+class|class|struct)\s+NAME\s*(?::[^;{]+)?\{')
    # R5 L7b: the anchored-level view is the read-only fact set the arm hook
    # sees, in this order; the kernel level is the last member so a host
    # reads the whole chronology (owner fill -> leg -> offset -> tick ->
    # kernel level) before it answers.
    anchored_view = re.sub(r'\s+', '', body(host, r'struct\s+NativeAnchoredLevelView\s*\{',
                                             'anchored level view'))
    if anchored_view != ('native_order::RequestHandleowner;std::uint64_towner_applied_ordinal=0;'
                         'std::uint64_towner_lot_incarnation=0;doubleowner_fill_price=0.0;'
                         'native_order::MatchCursorowner_cursor;native_order::RequestHandleleg;'
                         'native_order::Sideleg_side=native_order::Side::Long;'
                         'NativeAnchoredTriggertrigger=NativeAnchoredTrigger::Limit;'
                         'doubleoffset=0.0;doubleprice_tick=0.0;doublekernel_level=0.0;'):
        raise ValueError('NativeAnchoredLevelView must keep its exact read-only facts and order')
    begin_args = body(host, r'struct\s+NativeBeginArgs\s*\{', 'native begin args')
    begin_fields = (
        (r'\bconst\s+Bar\s*\*\s*bars\s*=\s*nullptr\s*;', 'bars'),
        (r'\bint\s+n\s*=\s*0\s*;', 'n'),
        (r'\bstd::string\s+input_tf\s*;', 'input_tf'),
        (r'\bstd::string\s+script_tf\s*;', 'script_tf'),
        (r'\bbool\s+bar_magnifier\s*=\s*false\s*;', 'bar_magnifier'),
        (r'\bint\s+magnifier_samples\s*=\s*4\s*;', 'magnifier_samples'),
        (r'\bMagnifierDistribution\s+magnifier_distribution\s*=\s*MagnifierDistribution::ENDPOINTS\s*;',
         'magnifier_distribution'),
        (r'\bbool\s+magnifier_volume_weighted\s*=\s*false\s*;', 'magnifier_volume_weighted'),
        (r'\bint\s+magnifier_volume_weighted_min_samples\s*=\s*2\s*;',
         'magnifier_volume_weighted_min_samples'),
        (r'\bint\s+magnifier_volume_weighted_max_samples\s*=\s*64\s*;',
         'magnifier_volume_weighted_max_samples'),
        (r'\bconst\s+InputsMap\s*\*\s*inputs\s*=\s*nullptr\s*;', 'inputs'),
        (r'\bconst\s+SymInfo\s*\*\s*syminfo\s*=\s*nullptr\s*;', 'syminfo'),
        (r'\bconst\s+void\s*\*\s*overrides_opaque\s*=\s*nullptr\s*;',
         'overrides_opaque'),
        (r'\bbool\s+is_stream\s*=\s*false\s*;', 'is_stream'),
        (r'\bint\s+warmup_n\s*=\s*0\s*;', 'warmup_n'),
    )
    positions = []
    for pattern, name in begin_fields:
        matches = list(re.finditer(pattern, begin_args))
        if len(matches) != 1:
            raise ValueError('NativeBeginArgs requires exactly one ' + name + ' field')
        positions.append(matches[0].start())
    if positions != sorted(positions):
        raise ValueError('NativeBeginArgs public begin fields changed order')
    input_context = body(host, r'struct\s+NativeInputContext\s*\{', 'native input context')
    compact_input_context = re.sub(r'\s+', '', input_context)
    for member in ('native_calendar::NativeIntervalinput_interval{};',
                   'native_calendar::NativeIntervalscript_interval{};',
                   'intinput_index=0;', 'boolcompletes_script_interval=false;'):
        if member not in compact_input_context:
            raise ValueError('NativeInputContext omits accepted-input fact: ' + member)
    tick_context = body(host, r'struct\s+NativeTickContext\s*\{', 'native tick context')
    compact_tick_context = re.sub(r'\s+', '', tick_context)
    for member in ('NativeDecisionContextdecision{};', 'std::uint64_tsequence=0;'):
        if member not in compact_tick_context:
            raise ValueError('NativeTickContext omits accepted-tick fact: ' + member)
    trail_state = body(host, r'struct\s+NativeTrailState\s*\{', 'native trail state')
    if re.sub(r'\s+', '', trail_state) != (
            'boolactivated=false;doublebest_price=0.0;doublecurrent_level=0.0;'
            'std::uint64_tactivation_ordinal=0;'):
        raise ValueError('NativeTrailState must expose the exact read-only A35 facts')
    require(host, ("NativeCurrentExecutionResult",), "engine_script_run_v19",
            r'\busing\s+NAME\s*=')
    require_exact_alias(
        host, "NativeCurrentExecutionResult",
        "std::variant<NativeCurrentRefusal,native_order::ExecutionAppliedEvent,"
        "native_order::NoEffectEvent,native_order::MatchRejectedEvent,"
        "native_order::CancelledEvent>", "engine_script_run_v19")
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
        (r'\bvirtual\s+void\s+prepare_native_begin\s*\('
         r'\s*const\s+NativeBeginArgs\s*&', "prepare_native_begin"),
        (r'\bvirtual\s+void\s+on_native_input\s*\('
         r'\s*const\s+Bar\s*&\s*,\s*const\s+NativeInputContext\s*&', "on_native_input"),
        (r'\bvirtual\s+void\s+on_native_tick\s*\('
         r'\s*const\s+Bar\s*&\s*,\s*const\s+NativeTickContext\s*&', "on_native_tick"),
        (r'\bvirtual\s+void\s+on_native_bar_open\s*\('
         r'\s*const\s+Bar\s*&', "on_native_bar_open"),
        (r'\bstd::optional\s*<\s*NativeTrailState\s*>\s+trail_state\s*\('
         r'\s*const\s+native_order::RequestHandle\s*&', "trail_state"),
        (r'\bvirtual\s+void\s+on_native_timeframe_bar\s*\('
         r'\s*const\s+Bar\s*&\s*,\s*const\s+NativeTimeframeBarContext\s*&',
         "on_native_timeframe_bar"),
        (r'\bstd::optional\s*<\s*Bar\s*>\s+native_series_bar\s*\('
         r'\s*std::size_t\s+\w+\s*\)\s*const\s*;', "native_series_bar"),
        # The begin-time declaration hook is NOT virtual: the host calls the
        # kernel, so it can never become another overridable entry point.
        (r'(?<!virtual )bool\s+declare_timeframe_subscriptions\s*\(\s*'
         r'std::vector\s*<\s*NativeTimeframeSubscription\s*>\s+\w+\s*\)\s*;',
         "declare_timeframe_subscriptions"),
        # So are the auxiliary feed's two doors: the begin-time declaration
        # and a realtime stream's append.
        (r'(?<!virtual )bool\s+declare_auxiliary_feed\s*\(\s*'
         r'std::optional\s*<\s*NativeAuxiliaryFeed\s*>\s+\w+\s*\)\s*;',
         "declare_auxiliary_feed"),
        (r'(?<!virtual )bool\s+append_auxiliary_bars\s*\(\s*const\s+Bar\s*\*\s*\w+\s*,'
         r'\s*std::size_t\s+\w+\s*\)\s*;', "append_auxiliary_bars"),
        (r'\bvirtual\s+std::optional\s*<\s*double\s*>\s+resolve_margin_call_units\s*\('
         r'\s*const\s+NativeMarginCallView\s*&', "resolve_margin_call_units"),
        (r'\bvirtual\s+void\s+on_native_margin_call\s*\('
         r'\s*const\s+native_order::MarginCallEvent\s*&', "on_native_margin_call"),
        (r'\bvirtual\s+std::optional\s*<\s*NativeMarginDecision\s*>'
         r'\s+resolve_margin_requirement\s*\('
         r'\s*const\s+NativeMarginRequirementView\s*&', "resolve_margin_requirement"),
        (r'\bvirtual\s+bool\s+margin_check_allowed\s*\('
         r'\s*const\s+NativeMarginCheckPoint\s*&', "margin_check_allowed"),
        (r'\bvirtual\s+std::optional\s*<\s*double\s*>\s+resolve_anchored_level\s*\('
         r'\s*const\s+NativeAnchoredLevelView\s*&', "resolve_anchored_level"),
        (r'\bstd::optional\s*<\s*double\s*>\s+native_liquidation_price\s*\('
         r'\s*\)\s*const\s*;', "native_liquidation_price"),
        (r'\bNativeRiskState\s+native_risk_state\s*\(\s*\)\s*const\s*;',
         "native_risk_state"),
        # N18: the open-lot snapshot is NOT virtual either — an observation of
        # the book the kernel holds, never a policy seam a host could override.
        (r'(?<!virtual )std::vector\s*<\s*NativeOpenLot\s*>\s+native_open_lots\s*\('
         r'\s*double\s+\w+\s*\)\s*const\s*;', "native_open_lots"),
        (r'\bvirtual\s+void\s+on_native_recalculate\s*\('
         r'\s*const\s+Bar\s*&\s*\w*\s*,\s*const\s+NativeDecisionContext\s*&',
         "on_native_recalculate"),
        (r'\bvirtual\s+void\s+on_native_sub_bar\s*\('
         r'\s*const\s+Bar\s*&\s*\w*\s*,\s*const\s+NativeDecisionContext\s*&',
         "on_native_sub_bar"),
        (r'\bstd::optional\s*<\s*Bar\s*>\s+current_partial_bar\s*\(\s*\)\s*const\s*;',
         "current_partial_bar"),
        # R5 D2-A: the two hook declarations are the host telling the kernel
        # what it implements -- never virtual, so neither can become another
        # overridable entry point.
        (r'(?<!virtual )void\s+declare_native_bar_open_hook\s*\(\s*bool\s+\w+\s*\)\s*;',
         "declare_native_bar_open_hook"),
        (r'(?<!virtual )void\s+declare_native_precommit_hook\s*\(\s*bool\s+\w+\s*\)\s*;',
         "declare_native_precommit_hook"),
    )
    for pattern, name in required_host_methods:
        if len(re.findall(pattern, host)) != 1:
            raise ValueError(name + " must be a v19 NativeStrategyHost member")
    for name in ('on_native_applied', 'current_execution_point', 'inspect_current_execution',
                 'execute_current', 'cohort_open', 'cohort_add', 'cohort_remove'):
        if name not in host:
            raise ValueError('missing current host contract: ' + name)
    if "native_failure_context_in_run" not in host:
        raise ValueError("native_failure_context_in_run must belong to engine_script_run_v19")
    if "native_failed_run_identity" not in host:
        raise ValueError("native_failed_run_identity must belong to engine_script_run_v19")
    if not re.search(r'\bSubmitResult\s+submit\s*\(\s*const\s+native_order::Request\s*&', host):
        raise ValueError("general submit must belong to engine_script_run_v19")
    if not re.search(r'\bReplaceResult\s+replace\s*\(\s*const\s+native_order::RequestHandle\s*&',
                     host):
        raise ValueError("general replace must belong to engine_script_run_v19")
    if "submit_market" not in host or "replace_market" not in host:
        raise ValueError("market-only submit/replace must remain in engine_script_run_v19")
    consumer = versioned(files[FILES[9]], "pineforge", "engine_script_run_v19")
    require(consumer, ("NativeExecutionConsumer",),
            "engine_script_run_v19", r'\bclass\s+NAME\s*')
    consumer_src = consumer_epoch_source(files)
    require(consumer_src,
            ("NativeStrategyHost::configure_native", "NativeStrategyHost::native_state",
             "NativeStrategyHost::native_events",
             "NativeStrategyHost::configure_native_fx_curve",
             "NativeStrategyHost::cohort_open", "NativeStrategyHost::cohort_add",
             "NativeStrategyHost::cohort_remove", "NativeStrategyHost::trail_state",
             "NativeStrategyHost::native_series_bar",
             "NativeStrategyHost::declare_timeframe_subscriptions",
             "NativeStrategyHost::declare_auxiliary_feed",
             "NativeStrategyHost::append_auxiliary_bars",
             "NativeStrategyHost::native_liquidation_price",
             "NativeStrategyHost::current_partial_bar",
             "NativeStrategyHost::native_recalculation_count",
             "NativeStrategyHost::native_recalculations_skipped",
             "NativeStrategyHost::native_risk_state",
             "NativeStrategyHost::native_sized_units",
             "NativeStrategyHost::native_open_lots",
             "NativeStrategyHost::declare_native_bar_open_hook",
             "NativeStrategyHost::declare_native_precommit_hook"),
            "engine_script_run_v19", r'\bNAME\s*\(')


    # These are continuation owners, not redundant physical-book snapshots.
    hash_requirements = {
        'hash_owner': ('native_order::BindOpenings', 'native_order::BindCohort',
                       'value.cycle', 'value.openings.size()', 'hash_handle(f, handle)',
                       'hash_cohort_handle(f, value.cohort)', 'unhashed native owner'),
        'hash_authority': ('native_order::OpeningsClose', 'value.cycle', 'value.side',
                           'value.openings.size()', 'hash_enrollment(value.enrollment)', 'hash_handle(f, handle)',
                           'native_order::CohortClose', 'hash_cohort_handle(f, value.cohort)',
                           'unhashed native authority'),
        'hash_scope': ('native_order::SelectedExposure', 'selected->cycle',
                       'selected->incarnations.size()', 'f.u(incarnation)'),
        'hash_current_point': ('point.decision.coordinate', 'point.decision.decision_floor_ms',
                              'point.decision.input_interval', 'point.decision.script_interval',
                              'point.decision.sub_index', 'point.decision.sub_count',
                              'point.decision.is_terminal_sub_bar', 'point.decision.sub_bar_open_ms',
                              'point.decision.script_bar_open_ms',
                              'point.decision.driver_statistics',
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
                 'notification.ordinal', 'notification.point', 'consuming_request_', 'draining_notifications_',
                 'preparing_begin_', 'callback_context_.sub_index',
                 'callback_context_.script_bar_open_ms', 'callback_context_.driver_statistics',
                 'input_callback_context_.has_value()', 'hash_input_context(f, *input_callback_context_)',
                 'input_callback_bar_.has_value()', 'hash_bar(f, *input_callback_bar_)',
                 'tick_callback_context_.has_value()', 'hash_tick_context(f, *tick_callback_context_)',
                 'tick_callback_bar_.has_value()', 'hash_bar(f, *tick_callback_bar_)',
                 'staged_ingress_fx_', 'driver_statistics_', 'hash_cohorts(f, requests_)'):
        if fact not in continuation:
            raise ValueError('native continuation omits current frame/queue fact: ' + fact)
    spec_hash = body(consumer_src, r'void\s+hash_spec\s*\([^)]*\)\s*noexcept\s*\{',
                     'native spec hash')
    if 'f.b(spec.timeframe_undetected);' not in spec_hash:
        raise ValueError('native continuation omits the undetected-timeframe spec fact')
    if 'f.u(static_cast<uint64_t>(spec.abort_reporting));' not in spec_hash:
        raise ValueError('native continuation omits abort-reporting policy')
    begin_guard = body(consumer_src,
                       r'bool\s+NativeExecutionConsumer::validate_undetected_begin\s*\([^)]*\)\s*\{',
                       'undetected-timeframe begin guard')
    for fact in ('has_undetected_timeframe()', 'args.n >= 2', 'args.is_stream'):
        if fact not in begin_guard:
            raise ValueError('undetected-timeframe begin guard omits: ' + fact)


def load(root=ROOT):
    return {name: (root / name).read_text() for name in FILES}


def check(root=ROOT):
    check_texts(load(root))
    check_fx_curve_introduced_at(authenticate_historical_host_manifests(root))


if __name__ == "__main__":
    if sys.argv[1:] == ['--self-test-bar-views']:
        self_test_bar_views()
        print('bar-view ownership, explicit slots and read inventory mutations ... OK')
        raise SystemExit(0)
    if sys.argv[1:] == ['--self-test-tempdir']:
        self_test_tempdir()
        print('historical provider extraction stays outside the repository ... OK')
        raise SystemExit(0)
    if len(sys.argv) != 1:
        raise SystemExit('usage: check_native_cpp_versions.py [--self-test-tempdir | --self-test-bar-views]')
    absent = missing_private_fixtures()
    if absent:
        # R2-F4: the text half reads only public sources, so it runs in the
        # fixture-absent tree TOO -- an unqualified 77 here silently dropped
        # every check_texts check from public PR CI. It runs first and its
        # failure is this guard's failure (exit 1, traceback, no skip); only
        # the manifest half -- which cannot run without the private headers --
        # is reported as the pinned skip.
        check_texts(load(ROOT))
        print(PRIVATE_FIXTURE_SKIP_LITERAL + str(absent[0].relative_to(ROOT)))
        print('native-version text checks ran and passed; the historical host '
              'manifests cannot be authenticated without that input')
        raise SystemExit(77)
    check()
    print("native_order identity v1 / values v7, native_calendar_v2, native_run_spec_v3, "
          "native_driver_v5, native_fx_curve_v1 and host engine_script_run_v19 ownership verified")
