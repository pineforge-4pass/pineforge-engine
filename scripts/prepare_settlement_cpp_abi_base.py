#!/usr/bin/env python3
"""Build an exact historical settlement ABI provider from local Git objects.

Run explicitly before CTest. This script never fetches Git history or dependencies.
The exact Git object and Eigen must already be available to the current build.
"""
from __future__ import annotations
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import tarfile

BASE_COMMIT = 'e60e57156a54bb1c74c0c0a4f5a338fd924c046d'
BASE_TREE = 'e209c89035ec884774f8f3e68e1dc118f26aaf51'
ROOT = Path(__file__).resolve().parents[1]
HEADER_MANIFEST = ROOT / 'tests/fixtures/settlement_cpp_abi/e60e571/manifest.json'
PRIOR_COMMIT = '0e18690db3fb6bb4705841c2a86936dfa206380a'
PRIOR_TREE = 'c29545d262bf6dcc22e46aa3382422f2e9c68378'
PRIOR_HEADER_MANIFEST = ROOT / 'tests/fixtures/settlement_cpp_abi/0e18690/manifest.json'
V13_COMMIT = 'c3ed45516721d3185fcd2f50bb293793304bc6e6'
V13_TREE = 'bb80c4767dddc0e5c9ae172672edd955ad344890'
V14_COMMIT = 'f736676ea9a558dc664b18f099a488b3a2c0067f'
V14_TREE = 'c69421f0f86d23aa48eeb2c79bf7f475a4db0e83'
V15_FROZEN_COMMIT = 'e7cdf052fa44d4c98035804db7b8399d3a5a37b2'
V15_FROZEN_TREE = 'dea028ca5664f78c055b1588820a4f7cce5b137f'
V16_FROZEN_COMMIT = 'ab9714beccb62b796c122cf68986ec9e7dbf4a67'
V16_FROZEN_TREE = '8c75db9858e63e019a31dd90230eff7f16ce24eb'
PROVIDERS = {
    # L0 freezes the exact pre-v17 provider.  This is deliberately a
    # same-epoch role: the matrix must prove both frozen/live v16 directions
    # link before L1 changes any public version literal.
    'v16-frozen': {'commit': V16_FROZEN_COMMIT, 'tree': V16_FROZEN_TREE,
                   'engine_epoch': 'engine_script_run_v16',
                   'manifest': ROOT / 'tests/fixtures/native_cpp_abi/host-ab9714b/manifest.json',
                   'default_output': 'native-abi-v16-frozen', 'headers_name': 'headers.tar'},
    'v15-frozen': {'commit': V15_FROZEN_COMMIT, 'tree': V15_FROZEN_TREE,
                   'engine_epoch': 'engine_script_run_v15',
                   'manifest': ROOT / 'tests/fixtures/native_cpp_abi/host-e7cdf05/manifest.json',
                   'default_output': 'native-abi-v15-frozen', 'headers_name': 'headers.tar'},
    'v14': {'commit': V14_COMMIT, 'tree': V14_TREE,
            'engine_epoch': 'engine_script_run_v14',
            'manifest': ROOT / 'tests/fixtures/native_cpp_abi/host-f736676/manifest.json',
            'default_output': 'native-abi-v14', 'headers_name': 'headers.tar'},
    'v13': {'commit': V13_COMMIT, 'tree': V13_TREE,
            'engine_epoch': 'engine_script_run_v13',
            'manifest': ROOT / 'tests/fixtures/native_cpp_abi/host-c3ed455/manifest.json',
            'default_output': 'native-abi-v13', 'headers_name': 'headers.tar'},
    'e60': {'commit': BASE_COMMIT, 'tree': BASE_TREE, 'manifest': HEADER_MANIFEST,
            'engine_epoch': 'engine_script_run_v13',
            'default_output': 'settlement-abi-base', 'headers_name': 'r2-headers.tar'},
    '0e': {'commit': PRIOR_COMMIT, 'tree': PRIOR_TREE, 'manifest': PRIOR_HEADER_MANIFEST,
           'engine_epoch': 'engine_script_run_v13',
           'default_output': 'settlement-abi-prior', 'headers_name': 'headers.tar'},
}
COPY_CACHE = (
    'CMAKE_BUILD_TYPE', 'CMAKE_C_COMPILER', 'CMAKE_CXX_COMPILER',
    'CMAKE_C_COMPILER_TARGET', 'CMAKE_CXX_COMPILER_TARGET',
    'CMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN', 'CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN',
    'CMAKE_C_COMPILER_LAUNCHER', 'CMAKE_CXX_COMPILER_LAUNCHER',
    'CMAKE_C_FLAGS', 'CMAKE_CXX_FLAGS',
    'CMAKE_C_FLAGS_DEBUG', 'CMAKE_CXX_FLAGS_DEBUG',
    'CMAKE_C_FLAGS_RELEASE', 'CMAKE_CXX_FLAGS_RELEASE',
    'CMAKE_C_FLAGS_RELWITHDEBINFO', 'CMAKE_CXX_FLAGS_RELWITHDEBINFO',
    'CMAKE_C_FLAGS_MINSIZEREL', 'CMAKE_CXX_FLAGS_MINSIZEREL',
    'CMAKE_EXE_LINKER_FLAGS', 'CMAKE_STATIC_LINKER_FLAGS',
    'CMAKE_POSITION_INDEPENDENT_CODE', 'CMAKE_INTERPROCEDURAL_OPTIMIZATION',
    'CMAKE_OSX_ARCHITECTURES', 'CMAKE_OSX_SYSROOT', 'CMAKE_OSX_DEPLOYMENT_TARGET',
    'CMAKE_SYSROOT', 'CMAKE_TOOLCHAIN_FILE', 'CMAKE_PREFIX_PATH', 'Eigen3_DIR',
    'CMAKE_DISABLE_FIND_PACKAGE_Eigen3', 'FETCHCONTENT_SOURCE_DIR_EIGEN',
    'PINEFORGE_ENABLE_SANITIZERS', 'PINEFORGE_ENABLE_COVERAGE',
    'PINEFORGE_VERSION_SOURCE',
)


def identity(path: Path) -> dict:
    raw = path.read_bytes()
    return {'sha256': hashlib.sha256(raw).hexdigest(), 'bytes': len(raw)}


def read_cache(path: Path) -> dict[str, str]:
    found = {}
    for line in path.read_text().splitlines():
        if line and not line.startswith(('#', '//')) and ':' in line and '=' in line:
            name_type, value = line.split('=', 1)
            found[name_type.split(':', 1)[0]] = value
    return found


def copied_cache(cache: dict[str, str]) -> dict[str, str]:
    return {key: cache[key] for key in COPY_CACHE if key in cache}


def refuse_existing_base(output: Path, reason: str) -> None:
    raise RuntimeError(f'{reason}; use a fresh --build-dir (refusing to delete {output})')


def receipt_matches_current(receipt: dict, current_cache: dict, compiler: dict, *,
                            commit: str = BASE_COMMIT, tree: str = BASE_TREE) -> None:
    """Raise if a prepared base must not be reused with this current build."""
    if receipt.get('schemaVersion') != 'pineforge-settlement-abi-base/v1':
        raise RuntimeError('existing ABI base receipt is not portable v1')
    if receipt.get('commit') != commit or receipt.get('tree') != tree:
        label = 'e60 R2' if commit == BASE_COMMIT else commit
        raise RuntimeError('existing ABI base does not pin ' + label)
    old = receipt.get('compiler') or {}
    for key in ('target', 'sha256', 'version'):
        if old.get(key) != compiler.get(key):
            raise RuntimeError('existing ABI base compiler implementation/version/target differs')
    if receipt.get('copiedCurrentCache') != copied_cache(current_cache):
        raise RuntimeError(
            'existing ABI base compiler/configuration/version-source/launcher differs from this build')


def reusable_prepared_base(output: Path, current_build: Path, *,
                           commit: str = BASE_COMMIT, tree: str = BASE_TREE) -> dict:
    """Return an existing receipt that is safe to reuse. Never deletes output."""
    receipt_path = output / 'receipt.json'
    if not output.exists():
        raise FileNotFoundError(str(output))
    if not receipt_path.is_file():
        refuse_existing_base(output, f'ABI base directory exists without a receipt at {receipt_path}')
    receipt = json.loads(receipt_path.read_text())
    cache = read_cache(current_build / 'CMakeCache.txt')
    compiler = compiler_identity(cache['CMAKE_CXX_COMPILER'])
    try:
        receipt_matches_current(receipt, cache, compiler, commit=commit, tree=tree)
    except RuntimeError as error:
        refuse_existing_base(output, str(error))
    resolve = lambda name: Path(name) if Path(name).is_absolute() else output / name
    library = resolve(receipt['archive'])
    headers = resolve(receipt.get('headers', 'r2-headers.tar'))
    if not library.is_file() or not headers.is_file():
        refuse_existing_base(output, 'existing ABI base is missing archive/header artifacts')
    if identity(library)['sha256'] != receipt['archiveSha256'] or identity(headers)['sha256'] != receipt['headersSha256']:
        refuse_existing_base(output, 'existing ABI base archive/header bytes do not match receipt')
    generated_name = receipt.get('generatedInclude')
    expected_generated = receipt.get('generatedHeaderSha256')
    if generated_name and expected_generated:
        generated = resolve(generated_name) / 'pineforge/version.h'
        if not generated.is_file() or identity(generated)['sha256'] != expected_generated:
            refuse_existing_base(output, 'existing ABI base generated version header does not match receipt')
    return receipt


def current_eigen_source(cache: dict[str, str], current: Path) -> Path | None:
    """Reuse the current build's populated dependency without a network fetch."""
    candidates = [cache.get(key) for key in
                  ('FETCHCONTENT_SOURCE_DIR_EIGEN', 'Eigen3_SOURCE_DIR', 'eigen_SOURCE_DIR')]
    candidates.append(str(Path(cache.get('FETCHCONTENT_BASE_DIR') or current / '_deps') / 'eigen-src'))
    for value in candidates:
        if not value:
            continue
        path = Path(value).resolve()
        if (path / 'CMakeLists.txt').is_file() and (path / 'Eigen/Core').is_file():
            return path
    return None


def run(argv, *, cwd=None, timeout=60, log: Path | None = None) -> bytes:
    result = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
    if log:
        log.write_bytes(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError(f'command failed ({result.returncode}): {shlex.join(map(str, argv))}\n'
                           + result.stderr.decode('utf-8', 'replace')[-6000:])
    return result.stdout


def extract_tar(raw: bytes, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=False)
    with tarfile.open(fileobj=io.BytesIO(raw)) as archive:
        members = archive.getmembers()
        seen = set()
        for member in members:
            path = Path(member.name)
            if path.is_absolute() or '..' in path.parts or member.name in seen:
                raise RuntimeError('unsafe or duplicate archive member: ' + member.name)
            if not member.isdir() and not member.isfile():
                raise RuntimeError('ABI preparation refuses archive links/special files: ' + member.name)
            seen.add(member.name)
        for member in members:
            target = destination / member.name
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                stream = archive.extractfile(member)
                assert stream is not None
                target.write_bytes(stream.read())
                target.chmod(member.mode & 0o777)


def authenticate_headers(directory: Path, manifest_path: Path = HEADER_MANIFEST, *,
                         commit: str = BASE_COMMIT, tree: str = BASE_TREE) -> dict:
    manifest = json.loads(manifest_path.read_text())
    label = 'R2' if commit == BASE_COMMIT else commit[:7]
    if manifest['commit'] != commit or manifest['tree'] != tree:
        raise RuntimeError('tracked ' + label + ' header pin changed')
    actual = {str(path.relative_to(directory)) for path in (directory / 'include').rglob('*') if path.is_file()}
    if actual != set(manifest['files']):
        raise RuntimeError(label + ' header closure differs from the tracked exact file inventory')
    for name, expected in manifest['files'].items():
        raw = (directory / name).read_bytes()
        blob = hashlib.sha1(b'blob ' + str(len(raw)).encode() + b'\0' + raw).hexdigest()
        if identity(directory / name) != {'sha256': expected['sha256'], 'bytes': expected['bytes']} or blob != expected['git_blob']:
            raise RuntimeError(label + ' header bytes differ: ' + name)
    return manifest


def compiler_identity(command: str) -> dict:
    resolved = shutil.which(command) if not Path(command).is_absolute() else command
    if not resolved:
        raise RuntimeError('compiler not found: ' + command)
    implementation = Path(resolved).resolve()
    if platform.system() == 'Darwin' and implementation.parent == Path('/usr/bin'):
        implementation = Path(run(['xcrun', '--find', Path(resolved).name]).decode().strip()).resolve()
    return {'command': str(resolved), 'implementation': str(implementation),
            **identity(implementation), 'version': run([command, '--version']).decode().strip(),
            'target': run([command, '-dumpmachine']).decode().strip()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-repo', type=Path, required=True)
    parser.add_argument('--current-build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--commit', default=BASE_COMMIT)
    parser.add_argument('--tree', default=BASE_TREE)
    parser.add_argument('--header-manifest', type=Path, default=HEADER_MANIFEST)
    args = parser.parse_args()
    provider = next((pin for pin in PROVIDERS.values()
                     if (pin['commit'], pin['tree']) == (args.commit, args.tree)), None)
    if provider is None:
        raise RuntimeError('unsupported historical ABI commit/tree pin')
    label = 'R2' if args.commit == BASE_COMMIT else args.commit[:7]
    repo, current, output = args.source_repo.resolve(), args.current_build.resolve(), args.output.resolve()
    if output.exists():
        raise RuntimeError('output must be new; reuse its existing receipt explicitly instead of rebuilding over it')
    if not 1 <= args.jobs <= 64:
        raise RuntimeError('--jobs must be 1..64')
    cache = read_cache(current / 'CMakeCache.txt')
    if Path(cache['CMAKE_HOME_DIRECTORY']).resolve() != repo:
        raise RuntimeError('current CMake cache belongs to a different source repository')
    try:
        commit = run(['git', '-C', str(repo), 'rev-parse', args.commit + '^{commit}']).decode().strip()
        tree = run(['git', '-C', str(repo), 'rev-parse', args.commit + '^{tree}']).decode().strip()
    except RuntimeError as error:
        raise RuntimeError(f'exact {label} source is unavailable; fetch the pinned object without tags '
                           f'before preparation: git fetch --no-tags --depth=1 origin {args.commit}') from error
    if commit != args.commit or tree != args.tree:
        raise RuntimeError(label + ' commit/tree mismatch')
    source_raw = run(['git', '-C', str(repo), 'archive', '--format=tar', args.commit,
                      'CMakeLists.txt', 'VERSION', 'cmake', 'include', 'src'])
    output.mkdir(parents=True)
    (output / 'source.tar').write_bytes(source_raw)
    source, build = output / 'source', output / 'build'
    extract_tar(source_raw, source)
    authenticate_headers(source, args.header_manifest, commit=args.commit, tree=args.tree)
    settings = copied_cache(cache)
    eigen_source = current_eigen_source(cache, current)
    if eigen_source is not None:
        settings['FETCHCONTENT_SOURCE_DIR_EIGEN'] = str(eigen_source)
    settings.update({'PINEFORGE_BUILD_TESTS': 'OFF', 'PINEFORGE_BUILD_TUTORIAL': 'OFF',
                     'PINEFORGE_BUILD_CORPUS_STRATEGIES': 'OFF', 'PINEFORGE_BUILD_LIVE_RUNNER': 'OFF',
                     'PINEFORGE_BUILD_BENCH_STRATEGIES': 'OFF', 'PINEFORGE_BUILD_SPEED_BENCH': 'OFF',
                     'CMAKE_EXPORT_COMPILE_COMMANDS': 'ON', 'FETCHCONTENT_FULLY_DISCONNECTED': 'ON'})
    configure = ['cmake', '-S', str(source), '-B', str(build), '-G', cache['CMAKE_GENERATOR'],
                 *[f'-D{key}={value}' for key, value in settings.items()]]
    run(configure, timeout=120, log=output / 'configure.log')
    build_argv = ['cmake', '--build', str(build), '--target', 'pineforge', '--parallel', str(args.jobs)]
    run(build_argv, timeout=1800, log=output / 'build.log')
    library = build / 'lib/libpineforge.a'
    if not library.read_bytes().startswith(b'!<arch>\n'):
        raise RuntimeError('base library is not a full static archive')
    archive_members = run(['ar', '-t', str(library)]).decode().splitlines()
    if len(archive_members) < 20:
        raise RuntimeError('base archive is not the full ' + label + ' library')
    headers = output / provider['headers_name']
    with tarfile.open(headers, 'w') as archive:
        archive.add(source / 'include', arcname='include', recursive=True)
    source_identity = {str(path.relative_to(source)): identity(path)['sha256']
                       for path in sorted(source.rglob('*')) if path.is_file()}
    receipt = {'schemaVersion': 'pineforge-settlement-abi-base/v1', 'commit': args.commit, 'tree': args.tree,
               'archive': str(library.relative_to(output)), 'archiveSha256': identity(library)['sha256'],
               'headers': headers.name, 'headersSha256': identity(headers)['sha256'],
               'generatedInclude': 'build/include',
               'generatedHeaderSha256': identity(build / 'include/pineforge/version.h')['sha256'],
               'sourceSha256': source_identity, 'sourceArchive': identity(output / 'source.tar'),
               'compiler': compiler_identity(cache['CMAKE_CXX_COMPILER']),
               'settings': settings, 'copiedCurrentCache': copied_cache(cache),
               'currentCacheSha256': identity(current / 'CMakeCache.txt')['sha256'],
               'baseCacheSha256': identity(build / 'CMakeCache.txt')['sha256'],
               'compileCommandsSha256': identity(build / 'compile_commands.json')['sha256'],
               'configureArgv': configure, 'buildArgv': build_argv, 'archiveMembers': archive_members,
               'executedBinaries': 0, 'networkFetches': 0,
               'sourceMode': 'immutable Git archive; generated version metadata uses VERSION fallback'}
    (output / 'receipt.json').write_text(json.dumps(receipt, indent=2, sort_keys=True) + '\n')
    print(str(output / 'receipt.json'))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
