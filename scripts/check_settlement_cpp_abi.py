#!/usr/bin/env python3
"""Actual old/new settlement ABI pairings. Compile and link; NEVER run callers.

Requires a separately prepared real e60 R2 archive. No Git/network/build fallback
is performed by this CTest-time checker. Existing native/script ABI guards stay
separate and mandatory, including their old epoch and sanitizer RTTI controls.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile

from check_aggregate_cpp_versions import clean, body
from check_native_cpp_abi import HOST_EVENTS_CALLER, assembly_layout_values
from prepare_settlement_cpp_abi_base import (
    BASE_COMMIT, BASE_TREE, COPY_CACHE, authenticate_headers, compiler_identity,
    extract_tar, identity, read_cache, run,
)

ROOT = Path(__file__).resolve().parents[1]
ENGINE = 'pineforge::engine_script_run_v13::BacktestEngine::'
OLD_METHODS = ('inspect_native_settlement', 'inspect_native_settlement_scoped',
               'settle_native_execution_at', 'settle_native_execution_scoped_at',
               'settle_resolved_execution','settle_execution_with_lifecycle','settle_with_context')
OLD_PRIVATE = ('add_to_pyramid_market', 'sequential_same_tick_reversal_fill')
NEW_METHODS = ('inspect_native_settlement_selected', 'settle_native_execution_selected_at',
               'settle_execution_selected_with_lifecycle', 'project_native_settlement_v1',
               'project_native_settlement_scoped_v1', 'project_native_settlement_selected_v1')
NEW_PRIVATE = ('add_to_pyramid_market_with_qty_provenance',
               'sequential_same_tick_reversal_fill_with_qty_provenance')
PRESERVED_ARCHIVE_SHA = 'e13d3d19ad4613c28beddfabb119f1dddadb2c39e474edd6304a2f75c7321f60'
PRESERVED_HEADERS_SHA = '1001102a496ae927ae98e111dd7dc68ab6c23ecc41a9eba00995144d9a532109'
FROZEN_NATIVE_HEADERS = ('native_order.hpp', 'native_order_identity.hpp', 'native_host.hpp',
                         'native_run_spec.hpp', 'market_driver.hpp', 'native_calendar.hpp',
                         'execution_consumer.hpp')

COMMON = '''#include <pineforge/native_host.hpp>
#include <cstddef>
#include <type_traits>
#include <utility>
using E = pineforge::engine_script_run_v13::BacktestEngine;
namespace ex = pineforge::execution;
using A = ex::Action;
using F = ex::Fill;
using C = ex::PhysicalExecutionContext;
using S = ex::CloseScope;
using I = ex::SettlementInspection;
using R = ex::Result;
using L = ex::LifecycleEffects;
static_assert(std::is_same_v<E, pineforge::BacktestEngine>);
static_assert(std::variant_size_v<S> == 2);
static_assert(std::variant_size_v<pineforge::native_order::CommandEvent> == 16);
'''
OLD_CALLER = COMMON + '''
static_assert(std::is_same_v<decltype(&E::inspect_native_settlement), I(E::*)(const A&,const F&) const>);
static_assert(std::is_same_v<decltype(&E::inspect_native_settlement_scoped), I(E::*)(const A&,const F&,S) const>);
static_assert(std::is_same_v<decltype(&E::settle_native_execution_at), R(E::*)(const A&,const F&,const C&)>);
static_assert(std::is_same_v<decltype(&E::settle_native_execution_scoped_at), R(E::*)(const A&,const F&,const C&,S)>);
static_assert(std::is_same_v<decltype(&E::settle_resolved_execution), R(E::*)(const A&,const F&)>);
static_assert(std::is_same_v<decltype(&E::settle_execution_with_lifecycle), R(E::*)(const A&,const F&,const L&)>);
static_assert(std::is_same_v<decltype(&E::settle_with_context), R(E::*)(const A&,const F&,const L&,const C&)>);
auto old_inspect = &E::inspect_native_settlement;
auto old_scoped_inspect = &E::inspect_native_settlement_scoped;
auto old_settle = &E::settle_native_execution_at;
auto old_scoped_settle = &E::settle_native_execution_scoped_at;
int main(int argc, char** argv) {
  auto* e = reinterpret_cast<E*>(argv);
  A a = ex::Flatten{}; F f{100,"","",1}; C c{}; S s = ex::Book{};
  auto i0 = (e->*old_inspect)(a,f); auto i1 = (e->*old_scoped_inspect)(a,f,s);
  auto r0 = (e->*old_settle)(a,f,c); auto r1 = (e->*old_scoped_settle)(a,f,c,s);
  auto p0=e->settle_resolved_execution(a,f); auto p1=e->settle_execution_with_lifecycle(a,f,L{});
  auto p2=e->settle_with_context(a,f,L{},c);
  return int(i0.closed_units+i1.closed_units+r0.closed_units+r1.closed_units+p0.closed_units+p1.closed_units+p2.closed_units);
}
'''
NEW_CALLER = COMMON + '''
using Set = ex::SelectedOpeningSet;
using P = ex::AccountEffectProjection;
static_assert(std::is_same_v<Set, ex::close_selection_v1::SelectedOpeningSet>);
static_assert(std::is_same_v<P, ex::settlement_projection_v1::AccountEffectProjection>);
static_assert(std::is_same_v<decltype(&E::inspect_native_settlement_selected), I(E::*)(const A&,const F&,const Set&) const>);
static_assert(std::is_same_v<decltype(&E::settle_native_execution_selected_at), R(E::*)(const A&,const F&,const C&,const Set&)>);
static_assert(std::is_same_v<decltype(&E::settle_execution_selected_with_lifecycle), R(E::*)(const A&,const F&,const L&,const Set&)>);
static_assert(std::is_same_v<decltype(&E::project_native_settlement_v1), P(E::*)(const A&,const F&) const>);
static_assert(std::is_same_v<decltype(&E::project_native_settlement_scoped_v1), P(E::*)(const A&,const F&,S) const>);
static_assert(std::is_same_v<decltype(&E::project_native_settlement_selected_v1), P(E::*)(const A&,const F&,const Set&) const>);
int main(int argc, char** argv) {
  auto* e = reinterpret_cast<E*>(argv);
  A a = ex::Flatten{}; F f{100,"","",1}; C c{}; L life{}; Set set{1,{1}};
  auto i = e->inspect_native_settlement_selected(a,f,set);
  auto r = e->settle_native_execution_selected_at(a,f,c,set);
  auto l = e->settle_execution_selected_with_lifecycle(a,f,life,set);
  auto p0 = e->project_native_settlement_v1(a,f);
  auto p1 = e->project_native_settlement_scoped_v1(a,f,ex::Book{});
  auto p2 = e->project_native_settlement_selected_v1(a,f,set);
  return int(i.closed_units+r.closed_units+l.closed_units+p0.realized_balance+p1.remaining_entry_cost+p2.marked_equity);
}
'''
PRIVATE_OLD_CALLER = COMMON + '''
using Add = void(E::*)(const std::string&,bool,double,double,int,pineforge::PositionSide,bool,uint64_t);
using Seq = void(E::*)(const std::string&,bool,double,double,int,uint64_t);
static_assert(std::is_same_v<decltype(&E::add_to_pyramid_market), Add>);
static_assert(std::is_same_v<decltype(&E::sequential_same_tick_reversal_fill), Seq>);
auto old_add = &E::add_to_pyramid_market;
auto old_sequential = &E::sequential_same_tick_reversal_fill;
int main(int argc,char** argv) {
  auto* e = reinterpret_cast<E*>(argv);
  (e->*old_add)("old",true,100,1,-1,pineforge::PositionSide::LONG,false,1);
  (e->*old_sequential)("old",true,100,1,-1,1);
  return argc;
}
'''
PRIVATE_NEW_CALLER = COMMON + '''
using Add = void(E::*)(const std::string&,bool,double,double,int,pineforge::PositionSide,bool,bool,uint64_t);
using Seq = void(E::*)(const std::string&,bool,double,double,int,bool,uint64_t);
static_assert(std::is_same_v<decltype(&E::add_to_pyramid_market_with_qty_provenance), Add>);
static_assert(std::is_same_v<decltype(&E::sequential_same_tick_reversal_fill_with_qty_provenance), Seq>);
int main(int argc,char** argv) {
  auto* e = reinterpret_cast<E*>(argv);
  e->add_to_pyramid_market_with_qty_provenance("new",true,100,1,-1,pineforge::PositionSide::LONG,false,true,1);
  e->sequential_same_tick_reversal_fill_with_qty_provenance("new",true,100,1,-1,true,1);
  return argc;
}
'''

FIELDS = {
    'Result': [('status','ex::Status'),('closed_units','double'),('opened_units','double'),
               ('current_ticket','double'),('first_trade_index','std::size_t'),
               ('closed_trade_count','std::size_t'),('opened_lot_incarnation','uint64_t')],
    'SettlementInspection': [('status','ex::Status'),('closed_units','double'),('opened_units','double'),
               ('resulting_abs_units','double'),('resulting_lot_count','std::size_t'),
               ('resulting_abs_notional','double'),('current_ticket','double'),('would_open','bool'),('incoming_short','bool')],
}


def normalized(text: str) -> str:
    return re.sub(r'\s+', ' ', clean(text)).strip()


def storage_declarations(header: str) -> list[str]:
    """Read top-level named data declarations; skip method bodies/declarations.

    Actual compiler offsets/types below supplement this source-order guard.
    String contents are irrelevant to storage declarations and may contain braces.
    """
    text = re.sub(r'"(?:\\.|[^"\\])*"', '""', clean(header))
    text = body(text, r'class\s+BacktestEngine\s*\{', 'BacktestEngine')
    statements, start, depth = [], 0, 0
    for at, char in enumerate(text):
        if char == '{':
            depth += 1
        elif char == '}':
            depth -= 1
            if depth == 0:
                prefix = text[start:at + 1]
                before = prefix.split('{', 1)[0]
                # Function definition (including const/noexcept) is not storage.
                if ')' in before and '=' not in before:
                    start = at + 1
        elif char == ';' and depth == 0:
            segment = text[start:at]
            start = at + 1
            segment = re.sub(r'\b(?:public|protected|private)\s*:', '', segment).strip()
            prefix = segment.split('=', 1)[0].split('{', 1)[0].strip()
            if '(' in prefix or re.match(r'^(?:using|typedef|friend|static_assert|class|struct|enum)\b', prefix):
                continue
            if re.search(r'\b[A-Za-z_]\w*\s*(?:\[[^]]*\])?\s*$', prefix):
                statements.append(re.sub(r'\s+', ' ', prefix))
    if len(statements) < 100:
        raise RuntimeError('engine storage inventory unexpectedly small; inspect source parser before accepting ABI')
    return statements


def frozen_shape(old_include: Path, current_include: Path) -> tuple[list[str], dict]:
    old_exec = (old_include/'pineforge/execution.hpp').read_text()
    cur_exec = (current_include/'pineforge/execution.hpp').read_text()
    for name in ['Result','SettlementInspection','Status','Fill','PhysicalExecutionContext','LifecycleEffects']:
        pattern = r'(?:struct|enum\s+class)\s+' + name + r'\s*\{'
        if normalized(body(old_exec, pattern, name)) != normalized(body(cur_exec, pattern, name)):
            raise RuntimeError('frozen execution aggregate/enum changed: ' + name)
    for name in FROZEN_NATIVE_HEADERS:
        if normalized((old_include/'pineforge'/name).read_text()) != normalized((current_include/'pineforge'/name).read_text()):
            raise RuntimeError('R3 must preserve native header layout/contracts: ' + name)
    scopes = [(directory/'pineforge/execution_close_scope.hpp').read_text()
              for directory in (old_include,current_include)]
    for name in ['Book','OpeningExposure']:
        pattern = r'struct\s+' + name + r'\s*\{'
        if normalized(body(scopes[0],pattern,name)) != normalized(body(scopes[1],pattern,name)):
            raise RuntimeError('frozen CloseScope alternative changed: ' + name)
    aliases = [re.search(r'using\s+CloseScope\s*=\s*[^;]+;',clean(text)) for text in scopes]
    if any(alias is None for alias in aliases) or normalized(aliases[0].group()) != normalized(aliases[1].group()):
        raise RuntimeError('CloseScope alternative identities/order changed')
    old_engine = (old_include/'pineforge/engine.hpp').read_text()
    cur_engine = (current_include/'pineforge/engine.hpp').read_text()
    old_storage, current_storage = storage_declarations(old_engine), storage_declarations(cur_engine)
    if old_storage != current_storage:
        raise RuntimeError('engine named data declarations/order changed')
    virtuals = lambda text: re.findall(r'\bvirtual\b[^;{]*(?:;|\{)', clean(text))
    if [normalized(v) for v in virtuals(old_engine)] != [normalized(v) for v in virtuals(cur_engine)]:
        raise RuntimeError('engine virtual method inventory changed')
    members = [re.search(r'\b([A-Za-z_]\w*)\s*(?:\[[^]]*\])?\s*$', declaration).group(1)
               for declaration in old_storage if not declaration.startswith('static ')]
    return members, {'engineStorage': old_storage, 'virtuals': [normalized(v) for v in virtuals(old_engine)]}


def layout_source(members: list[str]) -> tuple[str, int]:
    values = []
    assertions = []
    for name in ['Result','SettlementInspection']:
        values += [f'sizeof(ex::{name})',f'alignof(ex::{name})']
        for field, expected_type in FIELDS[name]:
            assertions.append(f'static_assert(std::is_same_v<decltype(ex::{name}::{field}),{expected_type}>);')
            values.append(f'offsetof(ex::{name},{field})')
    for index, status in enumerate(['Applied','NoEffect','InvalidPrice','InvalidQuantity','InvalidBook',
                                     'UnrepresentableQuantity','InvalidAccounting','InvalidLifecycle','InvalidCloseTarget']):
        assertions.append(f'static_assert(int(ex::Status::{status})=={index});')
    values += ['sizeof(ex::Status)','sizeof(S)','alignof(S)','std::variant_size_v<S>',
               'sizeof(E)','alignof(E)','sizeof(pineforge::PendingOrder)',
               'sizeof(pineforge::NativeStrategyHost)','sizeof(pineforge::NativeMarketEvent)',
               'sizeof(pineforge::NativeStateView)','sizeof(pineforge::native_order::Request)',
               'sizeof(pineforge::native_order::WorkingRequestCore)',
               'sizeof(pineforge::native_order::CommandEvent)']
    for member in members:
        values += [f'offsetof(E,{member})', f'sizeof(decltype(E::{member}))', f'alignof(decltype(E::{member}))']
    return COMMON + '\n'.join(assertions) + '\nextern "C" const unsigned long long abi_layout[] = {\n' + ',\n'.join(values) + '\n};\n', len(values)


def defined_symbols(library: Path) -> str:
    raw = run(['nm','-g','-C',str(library)]).decode('utf-8','replace')
    return '\n'.join(line for line in raw.splitlines() if re.search(r'\b[TWtw]\s+', line))


def validate_rejection(diagnostic: str, missing, domain=None) -> list[str]:
    symbols = re.findall(r'^\s*"(.+)", referenced from:', diagnostic, re.M)
    symbols += re.findall(r"undefined reference to [`'](.+)'", diagnostic)
    symbols += re.findall(r'undefined symbol:\s*(.+)', diagnostic)
    if not symbols:
        raise RuntimeError('link failure has no recognized undefined-symbol diagnostics')
    for method in missing:
        matching = [symbol for symbol in symbols if symbol.startswith(ENGINE+method+'(')]
        if not matching:
            raise RuntimeError('link failure omits expected undefined method: '+method)
        if domain and 'selected' in method and any(domain not in symbol for symbol in matching):
            raise RuntimeError('selected method has wrong/missing parameter namespace: '+method)
    unrelated = [symbol for symbol in symbols if not any(symbol.startswith(ENGINE+method+'(') for method in missing)]
    if unrelated:
        raise RuntimeError('link failure includes unrelated undefined symbols: '+', '.join(unrelated))
    return symbols


def load_base(args, destination: Path, current_cache: dict) -> tuple[Path, Path, Path, dict]:
    if not args.base_receipt.is_file():
        raise RuntimeError('real R2 archive receipt missing; run scripts/prepare_settlement_cpp_abi_base.py '
                           '--source-repo . --current-build BUILD --output BUILD/settlement-abi-base before CTest')
    receipt = json.loads(args.base_receipt.read_text())
    if receipt.get('commit') != BASE_COMMIT or receipt.get('tree') != BASE_TREE:
        raise RuntimeError('real base receipt does not pin e60 R2')
    base_root = args.base_receipt.resolve().parent
    resolve_artifact = lambda name: Path(name) if Path(name).is_absolute() else base_root/name
    library = resolve_artifact(receipt['archive'])
    headers = resolve_artifact(receipt.get('headers','r2-headers.tar'))
    if identity(library)['sha256'] != receipt['archiveSha256'] or identity(headers)['sha256'] != receipt['headersSha256']:
        raise RuntimeError('real R2 archive/header bytes do not match receipt')
    if not library.read_bytes().startswith(b'!<arch>\n'):
        raise RuntimeError('real base is not a static archive')
    if len(run(['ar','-t',str(library)]).splitlines()) < 20:
        raise RuntimeError('real base is not a full product archive')
    extract_tar(headers.read_bytes(), destination)
    authenticate_headers(destination)
    if receipt.get('schemaVersion') == 'pineforge-settlement-abi-base/v1':
        old_compiler = receipt['compiler']
        current_compiler = compiler_identity(args.compiler)
        if any(old_compiler[key] != current_compiler[key] for key in ('target','sha256','version')):
            raise RuntimeError('base compiler implementation/version/target differs; prepare base on this runner')
        actual_settings = {key: current_cache[key] for key in COPY_CACHE if key in current_cache}
        if receipt['copiedCurrentCache'] != actual_settings:
            raise RuntimeError('base preparation compiler/configuration differs from current build; prepare a matching base')
        generated = resolve_artifact(receipt['generatedInclude'])
        if identity(generated/'pineforge/version.h')['sha256'] != receipt['generatedHeaderSha256']:
            raise RuntimeError('base generated version header changed')
    else:
        # Explicit reuse of root's preserved Mac Release artifact, never a stub.
        if receipt['archiveSha256'] != PRESERVED_ARCHIVE_SHA or receipt['headersSha256'] != PRESERVED_HEADERS_SHA:
            raise RuntimeError('unrecognized legacy base receipt; use portable preparation')
        if args.extra_flag or current_cache.get('PINEFORGE_ENABLE_SANITIZERS') == 'ON' or current_cache.get('PINEFORGE_ENABLE_COVERAGE') == 'ON':
            raise RuntimeError('preserved uninstrumented R2 archive is not this lane; prepare a matching real base')
        if not args.base_generated_include:
            raise RuntimeError('preserved base requires --base-generated-include from its original build')
        generated = args.base_generated_include.resolve()
        identity(generated/'pineforge/version.h')
        old_cache=read_cache(generated.parent/'CMakeCache.txt')
        old_settings={key:old_cache[key] for key in COPY_CACHE if key in old_cache}
        current_settings={key:current_cache[key] for key in COPY_CACHE if key in current_cache}
        if old_settings != current_settings:
            raise RuntimeError('preserved R2 build configuration differs; use matching portable preparation')
        if identity(generated.parent/'lib/libpineforge.a')['sha256'] != receipt['archiveSha256']:
            raise RuntimeError('preserved base cache/header directory no longer belongs to its archive')
    symbols = defined_symbols(library)
    for method in (*OLD_METHODS,*OLD_PRIVATE):
        if ENGINE + method + '(' not in symbols:
            raise RuntimeError('real old archive omits original symbol: ' + method)
    for method in (*NEW_METHODS,*NEW_PRIVATE):
        if ENGINE + method + '(' in symbols:
            raise RuntimeError('supplied old archive already exports new method: ' + method)
    return library, destination/'include', generated, receipt


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--include', type=Path, required=True)
    parser.add_argument('--generated-include', type=Path, required=True)
    parser.add_argument('--base-receipt', type=Path, required=True)
    parser.add_argument('--base-generated-include', type=Path)
    parser.add_argument('--extra-flag', action='append', default=[])
    parser.add_argument('--receipt', type=Path, required=True)
    parser.add_argument('--base-only', action='store_true', help='preparation control only; does not claim new API proof')
    parser.add_argument('--old-rejections-only', action='store_true',
                        help='new public header callers versus actual old provider only; no current archive acceptance')
    parser.add_argument('--public-only', action='store_true',
                        help='complete public API matrix before new private helpers are integrated; partial R3 proof')
    args = parser.parse_args()
    library, include = args.library.resolve(), args.include.resolve()
    source = include.parent
    cache = read_cache(library.parent.parent/'CMakeCache.txt')
    if compiler_identity(cache['CMAKE_CXX_COMPILER'])['sha256'] != compiler_identity(args.compiler)['sha256']:
        raise RuntimeError('--compiler differs from current archive build compiler')
    if cache.get('PINEFORGE_ENABLE_SANITIZERS') == 'ON' and '-fsanitize=address,undefined' not in args.extra_flag:
        raise RuntimeError('sanitizer archive requires --extra-flag=-fsanitize=address,undefined')
    source_files = [source/'CMakeLists.txt', *[p for root in ('src','include','cmake') for p in (source/root).rglob('*') if p.is_file()]]
    stale = [str(path.relative_to(source)) for path in source_files if path.stat().st_mtime > library.stat().st_mtime]
    if sum((args.base_only,args.old_rejections_only,args.public_only)) > 1:
        raise RuntimeError('select only one partial proof mode')
    if stale and not args.old_rejections_only:
        raise RuntimeError('current archive predates source; full rebuild required: '+', '.join(stale))
    initial_identity = identity(library)
    flags = shlex.split(cache.get('CMAKE_CXX_FLAGS',''))
    common = [args.compiler,'-std=c++17',*flags,'-O0','-UNDEBUG','-ffp-contract=off',
              '-fno-access-control','-Wno-invalid-'+'offsetof',*args.extra_flag]
    if cache.get('PINEFORGE_ENABLE_SANITIZERS') == 'ON':
        common += ['-DEIGEN_MAX_ALIGN_BYTES=0']
    args.receipt.parent.mkdir(parents=True, exist_ok=True)
    log_root = Path(tempfile.mkdtemp(prefix=args.receipt.stem+'.artifacts-',dir=args.receipt.parent))
    report = {'schemaVersion':'pineforge-settlement-abi/v1','status':'incomplete',
              'mode':'base-only' if args.base_only else 'old-rejections-only' if args.old_rejections_only else 'public-only' if args.public_only else 'full-new-old-matrix',
              'currentArchive':{'path':str(library),**initial_identity},
              'compiler':compiler_identity(args.compiler),'callerFlags':common[1:],
              'privateAccessScaffolding':'-fno-access-control only for ABI TUs; product visibility unchanged',
              'artifactsDirectory':str(log_root),'newArchivePairingComplete':False,
              'sourceSha256':{str(path.relative_to(source)):identity(path)['sha256'] for path in source_files},
              'compiles':[],'links':[],'executedBinaries':0,'networkFetches':0}
    try:
        with tempfile.TemporaryDirectory(prefix='pineforge-settlement-abi-') as temporary:
            scratch = Path(temporary)
            old_library,old_include,old_generated,old_receipt = load_base(args,scratch/'r2',cache)
            report['base']={'receiptSha256':identity(args.base_receipt)['sha256'],
                            'archiveSha256':identity(old_library)['sha256'],
                            'commit':BASE_COMMIT,'tree':BASE_TREE}
            members,shape = frozen_shape(old_include,include)
            report['frozenShape']=shape

            def compile_tu(name,text,headers,generated):
                path=log_root/(name+'.cpp');path.write_text(text)
                obj=log_root/(name+'.o')
                argv=[*common,'-I',str(headers),'-I',str(generated),'-c',str(path),'-o',str(obj)]
                run(argv,timeout=120,log=log_root/(name+'.compile.log'))
                report['compiles'].append({'name':name,'argv':argv,'sourceSha256':identity(path)['sha256'],'objectSha256':identity(obj)['sha256']})
                return obj

            def link(name,obj,runtime,missing=(),domain=None):
                argv=[*common,str(obj),str(runtime),'-pthread','-o',str(log_root/name)]
                result=subprocess.run(argv,capture_output=True,text=True,timeout=120)
                diagnostic=result.stdout+result.stderr
                (log_root/(name+'.link.log')).write_text(diagnostic)
                if not missing:
                    if result.returncode: raise RuntimeError(name+' positive pair failed:\n'+diagnostic)
                else:
                    if result.returncode == 0: raise RuntimeError(name+' unexpectedly linked')
                    validate_rejection(diagnostic,missing,domain)
                report['links'].append({'name':name,'argv':argv,'exitCode':result.returncode,
                    'outcome':'expected-rejection' if missing else 'linked','requiredMissing':list(missing),
                    'selectionDomain':domain,'executed':False})

            # Compile every actual caller before interpreting any link outcome.
            old=compile_tu('old-book-singleton',OLD_CALLER,old_include,old_generated)
            cur_old=compile_tu('current-old-member-types',OLD_CALLER,include,args.generated_include)
            private_old=compile_tu('old-private-f8-f11',PRIVATE_OLD_CALLER,old_include,old_generated)
            current_private_old=compile_tu('current-old-private-types',PRIVATE_OLD_CALLER,include,args.generated_include)
            old_events=compile_tu('old-host-events-return',HOST_EVENTS_CALLER,old_include,old_generated)
            current_events=compile_tu('current-host-events-return',HOST_EVENTS_CALLER,include,args.generated_include)
            layout_text,word_count=layout_source(members)
            layouts=[]
            for label,headers,generated in [('old',old_include,old_generated),('current',include,args.generated_include)]:
                compile_tu(label+'-layout',layout_text,headers,generated)
                src=log_root/(label+'-layout.cpp');asm=log_root/(label+'-layout.s')
                run([*common,'-I',str(headers),'-I',str(generated),'-S',str(src),'-o',str(asm)],timeout=120,
                    log=log_root/(label+'-layout.assembly.log'))
                layouts.append(assembly_layout_values(asm.read_text(),word_count))
            if layouts[0]!=layouts[1]: raise RuntimeError('actual compiler old/current layout/offset/type-size arrays differ')
            report['layout']={'wordCount':word_count,'values':layouts[0],'members':members}
            if not args.base_only:
                current_header=clean((include/'pineforge/engine.hpp').read_text())
                for method in NEW_METHODS:
                    match=re.search(r'[^;{}]*\b'+method+r'\s*\([^;{}]*;',current_header)
                    if not match or re.search(r'\bvirtual\b',match.group()):
                        raise RuntimeError('new method must have one nonvirtual declaration: '+method)
                    preceding=current_header[:match.start()]
                    access=re.findall(r'\b(public|protected|private)\s*:',preceding)
                    if not access or access[-1]!='protected':
                        raise RuntimeError('new method must stay protected: '+method)
                new=compile_tu('new-six-methods',NEW_CALLER,include,args.generated_include)
                if not args.old_rejections_only and not args.public_only:
                    private_new=compile_tu('new-private-provenance',PRIVATE_NEW_CALLER,include,args.generated_include)
                wrong_project=scratch/'wrong-project';shutil.copytree(include,wrong_project)
                header=wrong_project/'pineforge/engine.hpp';changed=header.read_text();wrong_source=NEW_CALLER
                wrong_names=[]
                for method in NEW_METHODS[3:]:
                    future=method[:-2]+'v2';wrong_names.append(future)
                    changed=changed.replace(method,future);wrong_source=wrong_source.replace(method,future)
                header.write_text(changed)
                projection_header=wrong_project/'pineforge/execution_projection.hpp'
                projection_header.write_text(projection_header.read_text().replace('settlement_projection_v1','settlement_projection_v2'))
                wrong_source=wrong_source.replace('settlement_projection_v1','settlement_projection_v2')
                wrong_projection=compile_tu('synthetic-future-project-v2',wrong_source,wrong_project,args.generated_include)
                wrong_selection=scratch/'wrong-selection';shutil.copytree(include,wrong_selection)
                header=wrong_selection/'pineforge/execution_close_selection.hpp'
                changed=header.read_text();require_name='close_selection_v1'
                if require_name not in changed: raise RuntimeError('selection namespace missing from real header')
                header.write_text(changed.replace(require_name,'close_selection_v2'))
                wrong_set=compile_tu('synthetic-selection-v2',NEW_CALLER.replace(require_name,'close_selection_v2'),wrong_selection,args.generated_include)
            link('old-api-old-real',old,old_library)
            link('old-private-old-real',private_old,old_library)
            link('old-events-old-real',old_events,old_library)
            if not args.old_rejections_only:
                link('old-api-new-real',old,library)
                link('current-old-api-new-real',cur_old,library)
                link('old-private-new-real',private_old,library)
                link('current-old-private-new-real',current_private_old,library)
                link('old-events-new-real',old_events,library)
                link('current-events-new-real',current_events,library)
            if not args.base_only:
                link('new-api-old-real-rejected',new,old_library,NEW_METHODS,'close_selection_v1::SelectedOpeningSet')
                if not args.old_rejections_only:
                    link('new-api-new-real',new,library)
                    if not args.public_only:
                        link('new-private-new-real',private_new,library)
                        link('new-private-old-real-rejected',private_new,old_library,NEW_PRIVATE)
                    link('synthetic-project-v2-rejected',wrong_projection,library,wrong_names)
                    link('synthetic-selection-v2-rejected',wrong_set,library,
                         (NEW_METHODS[0],NEW_METHODS[1],NEW_METHODS[2],NEW_METHODS[5]),'close_selection_v2::SelectedOpeningSet')
            if identity(library)!=initial_identity: raise RuntimeError('current archive changed during ABI proof')
            if any(identity(source/name)['sha256']!=sha for name,sha in report['sourceSha256'].items()):
                raise RuntimeError('source changed during ABI proof')
            report['status']='base-control-only' if args.base_only else 'old-rejections-only' if args.old_rejections_only else 'public-matrix-only' if args.public_only else 'passed'
            report['newArchivePairingComplete']=report['status']=='passed'
    except Exception as error:
        report['status']='failed';report['error']=str(error)
        raise
    finally:
        args.receipt.write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')
    print(f"settlement ABI: {report['status']}; {len(report['compiles'])} compiles, {len(report['links'])} links, 0 executed binaries")
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
